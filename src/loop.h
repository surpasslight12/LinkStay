#ifndef LINKSTAY_LOOP_H
#define LINKSTAY_LOOP_H

/*
 * loop.h — minimal single-threaded event loop.
 *
 * Fixed-capacity fd slots and timer slots over poll(2), plus an integrated
 * signalfd channel. No allocation, no dynamic registration removal: callers
 * register everything up front and then run the loop.
 *
 * Timer contract: when a timer fires, its callback MUST either re-arm the
 * timer (ls_timer_step for periodic work) or disarm it (ls_timer_disarm),
 * otherwise the loop would spin on the still-elapsed timer.
 */

#include "base.h"

#include <poll.h>
#include <signal.h>

#define LS_LOOP_MAX_FDS 4U
#define LS_LOOP_MAX_TIMERS 4U

typedef struct ls_loop ls_loop_t;

typedef void (*ls_fd_cb_t)(ls_loop_t *loop, int fd, void *userdata);
typedef void (*ls_timer_cb_t)(ls_loop_t *loop, void *userdata);
typedef void (*ls_signal_cb_t)(ls_loop_t *loop, uint32_t signo,
                               void *userdata);

/* Timer slot. Contract: when a timer fires, its callback MUST re-arm
 * (ls_timer_step) or disarm (ls_timer_disarm) the timer; otherwise the
 * loop spins on the still-elapsed timer. */
typedef struct {
  uint64_t deadline_ms; /* UINT64_MAX = disarmed */
  ls_timer_cb_t cb;
  void *userdata;
} ls_timer_t;

struct ls_loop {
  const ls_log_t *log;
  uint64_t now_ms;
  bool stopping;
  int exit_code;

  struct pollfd fds[LS_LOOP_MAX_FDS];
  ls_fd_cb_t fd_cbs[LS_LOOP_MAX_FDS];
  void *fd_userdata[LS_LOOP_MAX_FDS];
  bool fd_critical[LS_LOOP_MAX_FDS];
  size_t fd_count;

  ls_timer_t timers[LS_LOOP_MAX_TIMERS];
  size_t timer_count;

  /* signalfd channel (optional; see ls_loop_watch_signals) */
  int signal_fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
  ls_signal_cb_t signal_cb;
  void *signal_userdata;
};

[[nodiscard]] bool ls_loop_init(ls_loop_t *restrict loop,
                                const ls_log_t *restrict log,
                                ls_err_t *restrict err);
void ls_loop_destroy(ls_loop_t *restrict loop);

/* Blocks the given signals process-wide and routes them through a signalfd
 * into `cb`. Must be called at most once per loop. */
[[nodiscard]] bool ls_loop_watch_signals(ls_loop_t *restrict loop,
                                         const int *restrict signals,
                                         size_t count, ls_signal_cb_t cb,
                                         void *userdata,
                                         ls_err_t *restrict err);

/* Registers a non-blocking fd for POLLIN dispatch. Set critical=true for
 * fds whose error (POLLERR/POLLHUP/POLLNVAL) should stop the loop
 * (e.g. signalfd); non-critical fd errors are logged but tolerated. */
[[nodiscard]] bool ls_loop_add_fd(ls_loop_t *restrict loop, int fd,
                                  ls_fd_cb_t cb, void *userdata, bool critical,
                                  ls_err_t *restrict err);

/* Claims a timer slot (initially disarmed). Returns false and sets err
 * when the timer slot array is full or arguments are invalid. */
[[nodiscard]] bool ls_loop_add_timer(ls_loop_t *restrict loop,
                                     ls_timer_cb_t cb, void *userdata,
                                     ls_timer_t **restrict out,
                                     ls_err_t *restrict err);

/* Runs until ls_loop_stop() or a fatal loop error; returns the exit code. */
int ls_loop_run(ls_loop_t *restrict loop);

void ls_loop_stop(ls_loop_t *restrict loop, int exit_code);

/* Monotonic timestamp refreshed once per loop iteration. */
[[nodiscard]] static inline uint64_t ls_loop_now(const ls_loop_t *loop) {
  return loop->now_ms;
}

/* ---- Timer operations (absolute deadline, UINT64_MAX = disarmed) ---- */

static inline void ls_timer_disarm(ls_timer_t *t) {
  t->deadline_ms = UINT64_MAX;
}

[[nodiscard]] static inline bool ls_timer_armed(const ls_timer_t *t) {
  return t->deadline_ms != UINT64_MAX;
}

static inline void ls_timer_arm_after(ls_timer_t *t, uint64_t base_ms,
                                      uint64_t delta_ms) {
  t->deadline_ms = ls_add_sat(base_ms, delta_ms);
}

/* Steps a periodic timer: keeps the original phase when possible, but never
 * schedules into the past. */
static inline void ls_timer_step(ls_timer_t *t, uint64_t interval_ms,
                                 uint64_t now_ms) {
  uint64_t base = ls_timer_armed(t) ? t->deadline_ms : now_ms;
  uint64_t next = ls_add_sat(base, interval_ms);
  if (next < now_ms) {
    next = ls_add_sat(now_ms, interval_ms);
  }
  t->deadline_ms = next;
}

#endif /* LINKSTAY_LOOP_H */
