#include "monitor.h"

#include "shutdown.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define LINKSTAY_PACKET_SIZE 64U
#define LINKSTAY_MAX_REPLY_DRAIN_PER_TICK 32U
#define POLL_FD_SIGNAL 0
#define POLL_FD_ICMP 1
#define POLL_FD_COUNT 2

/* ---- Timer primitive ----
 * Absolute deadline in monotonic ms; UINT64_MAX is the "disarmed" sentinel. */

typedef struct {
  uint64_t deadline_ms;
} timer_t_;

static inline void timer_clear(timer_t_ *t) { t->deadline_ms = UINT64_MAX; }
static inline bool timer_is_armed(const timer_t_ *t) {
  return t->deadline_ms != UINT64_MAX;
}
static inline bool timer_elapsed(const timer_t_ *t, uint64_t now_ms) {
  return timer_is_armed(t) && now_ms >= t->deadline_ms;
}

static inline uint64_t add_saturating(uint64_t a, uint64_t b) {
  uint64_t result = 0;
  return ckd_add(&result, a, b) ? UINT64_MAX : result;
}

static inline void timer_arm_after(timer_t_ *t, uint64_t base_ms,
                                   uint64_t delta_ms) {
  t->deadline_ms = add_saturating(base_ms, delta_ms);
}

/* Step a periodic timer: prefer the original phase, but never schedule in the
 * past. */
static inline void timer_step(timer_t_ *t, uint64_t interval_ms,
                              uint64_t now_ms) {
  uint64_t base = timer_is_armed(t) ? t->deadline_ms : now_ms;
  uint64_t next = add_saturating(base, interval_ms);
  if (next < now_ms) {
    next = add_saturating(now_ms, interval_ms);
  }
  t->deadline_ms = next;
}

static int timer_timeout_ms(const timer_t_ *t, uint64_t now_ms) {
  if (!timer_is_armed(t)) {
    return -1;
  }
  if (t->deadline_ms <= now_ms) {
    return 0;
  }
  uint64_t remaining = t->deadline_ms - now_ms;
  return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static int timer_min_timeout(int current, int candidate) {
  if (candidate < 0) {
    return current;
  }
  return current < 0 ? candidate
                     : (candidate < current ? candidate : current);
}

/* ---- Loop state ---- */

typedef struct {
  timer_t_ next_ping;
  timer_t_ reply_deadline;
  timer_t_ next_watchdog;
  uint64_t ping_send_time_ms;
  uint16_t expected_sequence;
  uint64_t interval_ms;
  uint64_t watchdog_interval_ms;
  bool watchdog_last_failed; /* throttles repeated "send failed" warnings */
} loop_state_t;

typedef enum {
  STEP_CONTINUE = 0,
  STEP_STOP = 1,
  STEP_ERROR = 2,
} step_result_t;

typedef struct {
  int fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
} signal_channel_t;

/* ---- Status formatting & error reporting ---- */

[[gnu::format(printf, 2, 3)]] static void
notify_statusf(linkstay_ctx_t *ctx, const char *fmt, ...) {
  if (!systemd_notifier_is_enabled(&ctx->systemd)) {
    return;
  }
  char status_msg[LINKSTAY_SYSTEMD_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(status_msg, sizeof(status_msg), fmt, args);
  va_end(args);
  (void)systemd_notifier_status(&ctx->systemd, status_msg);
}

[[gnu::format(printf, 2, 3)]] static step_result_t
runtime_error(linkstay_ctx_t *ctx, const char *fmt, ...) {
  char message[LINKSTAY_LOG_BUFFER_SIZE];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  logger_error(&ctx->logger, "%s", message);
  notify_statusf(ctx, "Error: %s", message);
  return STEP_ERROR;
}

/* ---- Ping bookkeeping ---- */

static bool ping_in_flight(const loop_state_t *state) {
  return timer_is_armed(&state->reply_deadline);
}

static void ping_arm(loop_state_t *state, uint64_t now_ms, uint64_t timeout_ms,
                     uint16_t expected_sequence) {
  timer_arm_after(&state->reply_deadline, now_ms, timeout_ms);
  state->ping_send_time_ms = now_ms;
  state->expected_sequence = expected_sequence;
}

static void ping_clear(loop_state_t *state) {
  timer_clear(&state->reply_deadline);
  state->ping_send_time_ms = 0;
  state->expected_sequence = 0;
}

static void log_statistics(linkstay_ctx_t *ctx) {
  const metrics_t *m = &ctx->metrics;
  if (m->successful_pings > 0) {
    logger_info(&ctx->logger,
                "Statistics: uptime %" PRIu64 "s | %" PRIu64 " pings, %" PRIu64
                " OK, %" PRIu64
                " failed (%.2f%% success) | latency min %.2fms, avg %.2fms, "
                "max %.2fms",
                metrics_uptime_seconds(m), m->total_pings, m->successful_pings,
                m->failed_pings, metrics_success_rate(m), m->min_latency,
                metrics_avg_latency(m), m->max_latency);
  } else {
    logger_info(&ctx->logger,
                "Statistics: uptime %" PRIu64 "s | %" PRIu64
                " pings, 0 OK, %" PRIu64
                " failed (0.00%% success) | latency N/A",
                metrics_uptime_seconds(m), m->total_pings, m->failed_pings);
  }
}

static void on_ping_success(linkstay_ctx_t *ctx,
                            const ping_result_t *result) {
  ctx->consecutive_fails = 0;
  metrics_record_success(&ctx->metrics, result->latency_ms);
  logger_debug(&ctx->logger,
               "Reply from %s: seq=%u time=%.2fms (avg %.2fms, %" PRIu64
               "/%" PRIu64 " OK, %.1f%% success)",
               ctx->config.target, (unsigned)result->sequence,
               result->latency_ms, metrics_avg_latency(&ctx->metrics),
               ctx->metrics.successful_pings, ctx->metrics.total_pings,
               metrics_success_rate(&ctx->metrics));
  notify_statusf(ctx,
                 "Online: %s up %" PRIu64 "s, last %.2fms, avg %.2fms, %" PRIu64
                 "/%" PRIu64 " OK (%.1f%%)",
                 ctx->config.target, metrics_uptime_seconds(&ctx->metrics),
                 result->latency_ms, metrics_avg_latency(&ctx->metrics),
                 ctx->metrics.successful_pings, ctx->metrics.total_pings,
                 metrics_success_rate(&ctx->metrics));
}

static void on_ping_failure(linkstay_ctx_t *ctx,
                            const ping_result_t *result) {
  ctx->consecutive_fails++;
  metrics_record_failure(&ctx->metrics);
  logger_warn(&ctx->logger, "No reply from %s: %s (failure %d of %d)",
              ctx->config.target, result->error_msg, ctx->consecutive_fails,
              ctx->config.fail_threshold);
  notify_statusf(ctx, "Unreachable: %s, %d/%d consecutive failures",
                 ctx->config.target, ctx->consecutive_fails,
                 ctx->config.fail_threshold);
}

/* ---- Shutdown FSM ---- */

static step_result_t handle_threshold_reached(linkstay_ctx_t *ctx) {
  if (ctx->consecutive_fails < ctx->config.fail_threshold) {
    return STEP_CONTINUE;
  }

  shutdown_result_t result = shutdown_trigger(&ctx->config, &ctx->logger);
  if (!ctx->config.poweroff) {
    logger_info(&ctx->logger,
                "Dry-run complete: simulated shutdown reached, exiting "
                "monitor loop");
    return STEP_STOP;
  }
  if (result != SHUTDOWN_RESULT_TRIGGERED) {
    logger_error(&ctx->logger,
                 "Shutdown command failed; continuing monitoring with failure "
                 "count preserved");
    return STEP_CONTINUE;
  }
  logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
  return STEP_STOP;
}

/* ---- Ping send/receive ---- */

static step_result_t send_ping(linkstay_ctx_t *ctx, loop_state_t *state,
                               uint64_t now_ms) {
  char error_buf[256];
  if (!icmp_pinger_send_echo(&ctx->pinger, &ctx->dest_addr, ctx->dest_addr_len,
                             ctx->cached_pid, LINKSTAY_PACKET_SIZE, error_buf,
                             sizeof(error_buf))) {
    return runtime_error(ctx, "Failed to send ICMP echo: %s", error_buf);
  }
  ping_arm(state, now_ms, (uint64_t)ctx->config.timeout_ms,
           ctx->pinger.sequence);
  return STEP_CONTINUE;
}

static step_result_t drain_icmp_replies(linkstay_ctx_t *ctx,
                                        loop_state_t *state, uint64_t now_ms) {
  ping_result_t reply;
  for (size_t i = 0; i < LINKSTAY_MAX_REPLY_DRAIN_PER_TICK; i++) {
    icmp_receive_status_t status = icmp_pinger_receive_reply(
        &ctx->pinger, &ctx->dest_addr, ctx->cached_pid,
        state->expected_sequence, state->ping_send_time_ms, now_ms, &reply);
    if (status == ICMP_RECEIVE_NO_MORE) {
      return STEP_CONTINUE;
    }
    if (status == ICMP_RECEIVE_ERROR) {
      ping_clear(state);
      return runtime_error(ctx, "ICMP receive failed: %s", reply.error_msg);
    }
    if (status == ICMP_RECEIVE_MATCHED && ping_in_flight(state)) {
      on_ping_success(ctx, &reply);
      ping_clear(state);
      return STEP_CONTINUE;
    }
  }
  return STEP_CONTINUE;
}

/* ---- Loop tick handlers ---- */

static void handle_watchdog(linkstay_ctx_t *ctx, loop_state_t *state,
                            uint64_t now_ms) {
  if (!timer_elapsed(&state->next_watchdog, now_ms)) {
    return;
  }
  bool sent = systemd_notifier_watchdog(&ctx->systemd);
  timer_step(&state->next_watchdog, state->watchdog_interval_ms, now_ms);
  /* Log only on the failure/recovery edge, not on every watchdog interval,
   * so a persistently broken notify socket does not spam the log. */
  if (!sent && !state->watchdog_last_failed) {
    logger_warn(&ctx->logger, "Failed to send systemd WATCHDOG notification");
  } else if (sent && state->watchdog_last_failed) {
    logger_info(&ctx->logger, "systemd WATCHDOG notification recovered");
  }
  state->watchdog_last_failed = !sent;
}

static step_result_t handle_ping_timeout(linkstay_ctx_t *ctx,
                                         loop_state_t *state,
                                         uint64_t now_ms) {
  if (!timer_elapsed(&state->reply_deadline, now_ms)) {
    return STEP_CONTINUE;
  }
  ping_result_t timeout_result = {.success = false,
                                  .latency_ms = 0.0,
                                  .error_msg = "ICMP reply deadline exceeded",
                                  .sequence = 0};
  on_ping_failure(ctx, &timeout_result);
  ping_clear(state);
  return handle_threshold_reached(ctx);
}

static step_result_t handle_scheduler(linkstay_ctx_t *ctx, loop_state_t *state,
                                      uint64_t now_ms) {
  if (ping_in_flight(state) || !timer_elapsed(&state->next_ping, now_ms)) {
    return STEP_CONTINUE;
  }
  step_result_t r = send_ping(ctx, state, now_ms);
  if (r != STEP_CONTINUE) {
    return r;
  }
  timer_step(&state->next_ping, state->interval_ms, now_ms);
  return STEP_CONTINUE;
}

/* ---- Signal channel ---- */

static bool signal_channel_init(signal_channel_t *ch, const logger_t *logger) {
  *ch = (signal_channel_t){.fd = -1};
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &mask, &ch->previous_mask) < 0) {
    logger_error(logger, "sigprocmask failed: %s", strerror(errno));
    return false;
  }
  ch->previous_mask_valid = true;
  ch->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (ch->fd < 0) {
    logger_error(logger, "signalfd failed: %s", strerror(errno));
    (void)sigprocmask(SIG_SETMASK, &ch->previous_mask, nullptr);
    ch->previous_mask_valid = false;
    return false;
  }
  return true;
}

static void signal_channel_destroy(signal_channel_t *ch,
                                   const logger_t *logger) {
  if (ch->fd >= 0) {
    close(ch->fd);
    ch->fd = -1;
  }
  if (ch->previous_mask_valid &&
      sigprocmask(SIG_SETMASK, &ch->previous_mask, nullptr) < 0 &&
      logger != nullptr) {
    logger_error(logger, "sigprocmask restore failed: %s", strerror(errno));
  }
  ch->previous_mask_valid = false;
}

static void handle_signal_fd(linkstay_ctx_t *ctx, int fd) {
  struct signalfd_siginfo info;
  if (read(fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
    return;
  }
  if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
    ctx->stop_flag = 1;
  } else if (info.ssi_signo == SIGUSR1) {
    log_statistics(ctx);
  }
}

/* ---- Reactor scaffolding ---- */

static bool refresh_now(uint64_t *now_ms) {
  uint64_t t = get_monotonic_ms();
  if (t == UINT64_MAX) {
    return false;
  }
  *now_ms = t;
  return true;
}

static void log_startup(linkstay_ctx_t *ctx) {
  logger_info(&ctx->logger,
              "linkstay %s monitoring %s | interval %ds, timeout %dms, "
              "threshold %d, poweroff %s",
              LINKSTAY_VERSION, ctx->config.target, ctx->config.interval_sec,
              ctx->config.timeout_ms, ctx->config.fail_threshold,
              ctx->config.poweroff ? "true" : "false");
  if (systemd_notifier_is_enabled(&ctx->systemd) &&
      !systemd_notifier_ready(&ctx->systemd)) {
    logger_warn(&ctx->logger, "Failed to send systemd READY notification");
  }
  notify_statusf(ctx, "Monitoring %s every %ds (poweroff %s)",
                 ctx->config.target, ctx->config.interval_sec,
                 ctx->config.poweroff ? "true" : "false");
}

static void log_shutdown(linkstay_ctx_t *ctx, int exit_code) {
  if (ctx->stop_flag) {
    logger_info(&ctx->logger, "Shutdown signal received, stopping gracefully");
    if (systemd_notifier_is_enabled(&ctx->systemd)) {
      (void)systemd_notifier_stopping(&ctx->systemd);
    }
  }
  log_statistics(ctx);
  if (exit_code == LINKSTAY_EXIT_SUCCESS) {
    logger_info(&ctx->logger, "linkstay monitor stopped");
  }
}

static int compute_poll_timeout(const loop_state_t *state, uint64_t now_ms) {
  int timeout = ping_in_flight(state)
                    ? timer_timeout_ms(&state->reply_deadline, now_ms)
                    : timer_timeout_ms(&state->next_ping, now_ms);
  return timer_min_timeout(
      timeout, timer_timeout_ms(&state->next_watchdog, now_ms));
}

/* ---- Public API ---- */

bool linkstay_ctx_init(linkstay_ctx_t *restrict ctx,
                       const config_t *restrict config,
                       char *restrict error_msg, size_t error_size) {
  if (ctx == nullptr || config == nullptr || error_msg == nullptr || error_size == 0) {
    return false;
  }

  *ctx = (linkstay_ctx_t){
      .config = *config,
      .cached_pid = (uint16_t)(getpid() & 0xFFFF),
  };
  if (ctx->cached_pid == 0) {
    ctx->cached_pid = 1;
  }

  logger_init(&ctx->logger, ctx->config.log_level,
              config_log_timestamps_enabled(&ctx->config));
  if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
    config_print(&ctx->config, &ctx->logger);
  }

  ctx->dest_addr_len = sizeof(ctx->dest_addr);
  if (!icmp_resolve_target(ctx->config.target, &ctx->dest_addr,
                           &ctx->dest_addr_len, error_msg, error_size)) {
    return false;
  }

  int family = ((const struct sockaddr *)&ctx->dest_addr)->sa_family;
  if (!icmp_pinger_init(&ctx->pinger, family, error_msg, error_size)) {
    return false;
  }
  if (ctx->pinger.bpf_filter_errno != 0) {
    logger_warn(&ctx->logger,
                "ICMP BPF filter unavailable for %s traffic: %s; continuing "
                "without kernel-side packet filtering",
                family == AF_INET6 ? "IPv6" : "IPv4",
                strerror(ctx->pinger.bpf_filter_errno));
  } else if (ctx->pinger.bpf_filter_attached) {
    logger_debug(&ctx->logger, "ICMP BPF filter active for %s traffic",
                 family == AF_INET6 ? "IPv6" : "IPv4");
  }

  metrics_init(&ctx->metrics);

  ctx->systemd.sockfd = -1;
  if (ctx->config.enable_systemd) {
    systemd_notifier_init(&ctx->systemd);
  }

  if (!systemd_notifier_is_enabled(&ctx->systemd)) {
    logger_debug(&ctx->logger,
                 "systemd integration inactive (NOTIFY_SOCKET not set)");
    return true;
  }
  uint64_t watchdog_ms = systemd_notifier_watchdog_interval_ms(&ctx->systemd);
  if (watchdog_ms > 0) {
    logger_debug(&ctx->logger,
                 "systemd integration active, watchdog ping every %" PRIu64
                 "ms",
                 watchdog_ms);
  } else {
    logger_debug(&ctx->logger,
                 "systemd integration active, watchdog disabled");
  }
  return true;
}

void linkstay_ctx_destroy(linkstay_ctx_t *restrict ctx) {
  if (ctx == nullptr) {
    return;
  }
  systemd_notifier_destroy(&ctx->systemd);
  icmp_pinger_destroy(&ctx->pinger);
  *ctx = (linkstay_ctx_t){};
}

int linkstay_reactor_run(linkstay_ctx_t *restrict ctx) {
  if (ctx == nullptr) {
    return LINKSTAY_EXIT_FAILURE;
  }

  signal_channel_t signals;
  if (!signal_channel_init(&signals, &ctx->logger)) {
    return LINKSTAY_EXIT_FAILURE;
  }

  uint64_t now_ms;
  if (!refresh_now(&now_ms)) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    signal_channel_destroy(&signals, &ctx->logger);
    return LINKSTAY_EXIT_FAILURE;
  }
  uint64_t interval_ms =
      (uint64_t)ctx->config.interval_sec * LINKSTAY_MS_PER_SEC;

  loop_state_t state = {
      .next_ping = {.deadline_ms = now_ms}, /* fire first ping immediately */
      .reply_deadline = {.deadline_ms = UINT64_MAX},
      .next_watchdog = {.deadline_ms = UINT64_MAX},
      .interval_ms = interval_ms,
      .watchdog_interval_ms =
          systemd_notifier_watchdog_interval_ms(&ctx->systemd),
  };
  if (state.watchdog_interval_ms > 0) {
    timer_arm_after(&state.next_watchdog, now_ms, state.watchdog_interval_ms);
  }

  struct pollfd fds[POLL_FD_COUNT] = {
      [POLL_FD_SIGNAL] = {.fd = signals.fd, .events = POLLIN},
      [POLL_FD_ICMP] = {.fd = ctx->pinger.sockfd, .events = POLLIN},
  };

  log_startup(ctx);

  int exit_code = LINKSTAY_EXIT_SUCCESS;
  while (!ctx->stop_flag) {
    if (!refresh_now(&now_ms)) {
      runtime_error(ctx, "Failed to refresh monotonic clock");
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    handle_watchdog(ctx, &state, now_ms);
    step_result_t r = handle_ping_timeout(ctx, &state, now_ms);
    if (r == STEP_CONTINUE) {
      r = handle_scheduler(ctx, &state, now_ms);
    }
    if (r == STEP_STOP) {
      break;
    }
    if (r == STEP_ERROR) {
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    int poll_result = poll(fds, POLL_FD_COUNT, compute_poll_timeout(&state,
                                                                    now_ms));
    if (poll_result < 0 && errno != EINTR) {
      logger_error(&ctx->logger, "poll error: %s", strerror(errno));
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    if (!refresh_now(&now_ms)) {
      runtime_error(ctx, "Failed to refresh monotonic clock");
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    const short error_events = POLLERR | POLLHUP | POLLNVAL;
    if (fds[POLL_FD_SIGNAL].revents & error_events) {
      logger_error(&ctx->logger, "Signal fd entered error state");
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }
    if (fds[POLL_FD_ICMP].revents & error_events) {
      logger_error(&ctx->logger, "ICMP socket entered error state");
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    if (fds[POLL_FD_SIGNAL].revents & POLLIN) {
      handle_signal_fd(ctx, signals.fd);
    }
    if (fds[POLL_FD_ICMP].revents & POLLIN &&
        drain_icmp_replies(ctx, &state, now_ms) == STEP_ERROR) {
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }

    fds[POLL_FD_SIGNAL].revents = 0;
    fds[POLL_FD_ICMP].revents = 0;
  }

  log_shutdown(ctx, exit_code);
  signal_channel_destroy(&signals, &ctx->logger);
  return exit_code;
}
