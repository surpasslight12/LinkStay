#include "loop.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* ---- Setup & teardown ---- */

bool ls_loop_init(ls_loop_t *restrict loop, const ls_log_t *restrict log,
                  ls_err_t *restrict err) {
  if (loop == nullptr) {
    return ls_err_set(err, "Event loop pointer is null");
  }
  *loop = (ls_loop_t){
      .log = log,
      .exit_code = LS_EXIT_SUCCESS,
      .signal_fd = -1,
  };
  for (size_t i = 0; i < LS_LOOP_MAX_TIMERS; i++) {
    ls_timer_disarm(&loop->timers[i]);
  }
  return true;
}

void ls_loop_destroy(ls_loop_t *restrict loop) {
  if (loop == nullptr) {
    return;
  }
  if (loop->signal_fd >= 0) {
    close(loop->signal_fd);
    loop->signal_fd = -1;
  }
  if (loop->previous_mask_valid &&
      sigprocmask(SIG_SETMASK, &loop->previous_mask, nullptr) < 0) {
    ls_error(loop->log, "sigprocmask restore failed: %s", strerror(errno));
  }
  loop->previous_mask_valid = false;
}

/* ---- Registration ---- */

static void dispatch_signal_fd(ls_loop_t *loop, int fd, void *userdata) {
  (void)userdata;
  struct signalfd_siginfo info;
  if (read(fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
    return;
  }
  if (loop->signal_cb != nullptr) {
    loop->signal_cb(loop, info.ssi_signo, loop->signal_userdata);
  }
}

bool ls_loop_watch_signals(ls_loop_t *restrict loop,
                           const int *restrict signals, size_t count,
                           ls_signal_cb_t cb, void *userdata,
                           ls_err_t *restrict err) {
  if (loop == nullptr || signals == nullptr || cb == nullptr ||
      loop->signal_fd >= 0) {
    return ls_err_set(err, "Invalid signal watch request");
  }

  sigset_t mask;
  sigemptyset(&mask);
  for (size_t i = 0; i < count; i++) {
    sigaddset(&mask, signals[i]);
  }
  if (sigprocmask(SIG_BLOCK, &mask, &loop->previous_mask) < 0) {
    return ls_err_set(err, "sigprocmask failed: %s", strerror(errno));
  }
  loop->previous_mask_valid = true;

  loop->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (loop->signal_fd < 0) {
    int saved_errno = errno;
    (void)sigprocmask(SIG_SETMASK, &loop->previous_mask, nullptr);
    loop->previous_mask_valid = false;
    return ls_err_set(err, "signalfd failed: %s", strerror(saved_errno));
  }

  loop->signal_cb = cb;
  loop->signal_userdata = userdata;
  if (!ls_loop_add_fd(loop, loop->signal_fd, dispatch_signal_fd, nullptr)) {
    return ls_err_set(err, "Event loop fd capacity exceeded");
  }
  return true;
}

bool ls_loop_add_fd(ls_loop_t *restrict loop, int fd, ls_fd_cb_t cb,
                    void *userdata) {
  if (loop == nullptr || fd < 0 || cb == nullptr ||
      loop->fd_count >= LS_LOOP_MAX_FDS) {
    return false;
  }
  size_t slot = loop->fd_count++;
  loop->fds[slot] = (struct pollfd){.fd = fd, .events = POLLIN};
  loop->fd_cbs[slot] = cb;
  loop->fd_userdata[slot] = userdata;
  return true;
}

ls_timer_t *ls_loop_add_timer(ls_loop_t *restrict loop, ls_timer_cb_t cb,
                              void *userdata) {
  if (loop == nullptr || cb == nullptr ||
      loop->timer_count >= LS_LOOP_MAX_TIMERS) {
    return nullptr;
  }
  ls_timer_t *timer = &loop->timers[loop->timer_count++];
  timer->cb = cb;
  timer->userdata = userdata;
  ls_timer_disarm(timer);
  return timer;
}

/* ---- Run ---- */

void ls_loop_stop(ls_loop_t *restrict loop, int exit_code) {
  if (loop == nullptr) {
    return;
  }
  loop->stopping = true;
  loop->exit_code = exit_code;
}

static bool refresh_now(ls_loop_t *loop) {
  uint64_t now = ls_now_ms();
  if (LS_UNLIKELY(now == UINT64_MAX)) {
    ls_error(loop->log, "Failed to refresh monotonic clock");
    ls_loop_stop(loop, LS_EXIT_FAILURE);
    return false;
  }
  loop->now_ms = now;
  return true;
}

/* Fires every elapsed timer once, in registration order. */
static void fire_timers(ls_loop_t *loop) {
  for (size_t i = 0; i < loop->timer_count && !loop->stopping; i++) {
    ls_timer_t *timer = &loop->timers[i];
    if (ls_timer_armed(timer) && loop->now_ms >= timer->deadline_ms) {
      timer->cb(loop, timer->userdata);
    }
  }
}

/* Poll timeout: milliseconds until the nearest armed deadline, or -1. */
static int poll_timeout_ms(const ls_loop_t *loop) {
  uint64_t nearest = UINT64_MAX;
  for (size_t i = 0; i < loop->timer_count; i++) {
    if (loop->timers[i].deadline_ms < nearest) {
      nearest = loop->timers[i].deadline_ms;
    }
  }
  if (nearest == UINT64_MAX) {
    return -1;
  }
  if (nearest <= loop->now_ms) {
    return 0;
  }
  uint64_t remaining = nearest - loop->now_ms;
  return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static void dispatch_fds(ls_loop_t *loop) {
  const short error_events = POLLERR | POLLHUP | POLLNVAL;
  for (size_t i = 0; i < loop->fd_count && !loop->stopping; i++) {
    short revents = loop->fds[i].revents;
    loop->fds[i].revents = 0;
    if (revents & error_events) {
      ls_error(loop->log, "fd %d entered error state", loop->fds[i].fd);
      ls_loop_stop(loop, LS_EXIT_FAILURE);
      return;
    }
    if (revents & POLLIN) {
      loop->fd_cbs[i](loop, loop->fds[i].fd, loop->fd_userdata[i]);
    }
  }
}

int ls_loop_run(ls_loop_t *restrict loop) {
  if (loop == nullptr) {
    return LS_EXIT_FAILURE;
  }

  while (!loop->stopping) {
    if (!refresh_now(loop)) {
      break;
    }

    fire_timers(loop);
    if (loop->stopping) {
      break;
    }

    int poll_result = poll(loop->fds, (nfds_t)loop->fd_count,
                           poll_timeout_ms(loop));
    if (poll_result < 0 && errno != EINTR) {
      ls_error(loop->log, "poll error: %s", strerror(errno));
      ls_loop_stop(loop, LS_EXIT_FAILURE);
      break;
    }

    if (!refresh_now(loop)) {
      break;
    }
    if (poll_result > 0) {
      dispatch_fds(loop);
    }
  }

  return loop->exit_code;
}
