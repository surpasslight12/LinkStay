#include "monitor.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* ---- Types ---- */

typedef struct {
  uint64_t deadline_ms;
  bool armed;
} monitor_timer_t;

typedef struct {
  monitor_timer_t reply_timer;
  uint64_t send_time_ms;
  uint16_t expected_sequence;
} monitor_ping_state_t;

typedef struct {
  monitor_timer_t countdown_timer;
} monitor_shutdown_state_t;

typedef struct {
  monitor_timer_t next_ping_timer;
  uint64_t interval_ms;
} monitor_scheduler_state_t;

typedef struct {
  monitor_timer_t next_notify_timer;
  uint64_t interval_ms;
} monitor_watchdog_state_t;

typedef struct {
  monitor_ping_state_t ping;
  monitor_shutdown_state_t shutdown;
  monitor_scheduler_state_t scheduler;
  monitor_watchdog_state_t watchdog;
} monitor_state_t;

typedef enum {
  MONITOR_STEP_CONTINUE = 0,
  MONITOR_STEP_STOP = 1,
  MONITOR_STEP_ERROR = 2,
} monitor_step_result_t;

#define LINKSTAY_PACKET_SIZE 64U
#define LINKSTAY_MAX_REPLY_DRAIN_PER_TICK 32U

/* ---- Metrics — static ---- */

static void metrics_init(metrics_t *metrics) {
  if (metrics == NULL) {
    return;
  }
  metrics->total_pings = 0;
  metrics->successful_pings = 0;
  metrics->failed_pings = 0;
  metrics->total_latency = 0.0;
  metrics->min_latency = -1.0;
  metrics->max_latency = -1.0;
  metrics->start_time_ms = get_monotonic_ms();
}

static void metrics_record_success(metrics_t *metrics, double latency_ms) {
  if (LINKSTAY_UNLIKELY(metrics == NULL)) {
    return;
  }
  if (LINKSTAY_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  metrics->total_pings++;
  metrics->successful_pings++;
  metrics->total_latency += latency_ms;
  if (LINKSTAY_UNLIKELY(metrics->min_latency < 0.0) ||
      latency_ms < metrics->min_latency) {
    metrics->min_latency = latency_ms;
  }
  if (LINKSTAY_UNLIKELY(metrics->max_latency < 0.0) ||
      latency_ms > metrics->max_latency) {
    metrics->max_latency = latency_ms;
  }
}

static void metrics_record_failure(metrics_t *metrics) {
  if (LINKSTAY_UNLIKELY(metrics == NULL)) {
    return;
  }
  metrics->total_pings++;
  metrics->failed_pings++;
}

static double metrics_success_rate(const metrics_t *metrics) {
  if (metrics == NULL || metrics->total_pings == 0) {
    return 0.0;
  }
  return (double)metrics->successful_pings / (double)metrics->total_pings *
         100.0;
}

static double metrics_avg_latency(const metrics_t *metrics) {
  if (metrics == NULL || metrics->successful_pings == 0) {
    return 0.0;
  }
  return metrics->total_latency / (double)metrics->successful_pings;
}

static uint64_t metrics_uptime_seconds(const metrics_t *metrics) {
  if (metrics == NULL) {
    return 0;
  }
  uint64_t now_ms = get_monotonic_ms();
  if (now_ms == UINT64_MAX || metrics->start_time_ms == UINT64_MAX ||
      now_ms < metrics->start_time_ms) {
    return 0;
  }
  return (now_ms - metrics->start_time_ms) / LINKSTAY_MS_PER_SEC;
}

/* ---- Monitor state — static ---- */

static uint64_t monitor_deadline_add_ms(uint64_t base_ms, uint64_t delta_ms) {
  uint64_t result = 0;
  if (LINKSTAY_UNLIKELY(ckd_add(&result, base_ms, delta_ms))) {
    return UINT64_MAX;
  }
  return result;
}

static void monitor_timer_clear(monitor_timer_t *restrict timer) {
  if (timer == NULL) {
    return;
  }

  timer->deadline_ms = 0;
  timer->armed = false;
}

static bool monitor_timer_arm_absolute(monitor_timer_t *restrict timer,
                                       uint64_t deadline_ms) {
  if (timer == NULL || deadline_ms == UINT64_MAX) {
    return false;
  }

  timer->deadline_ms = deadline_ms;
  timer->armed = true;
  return true;
}

static bool monitor_timer_arm_after(monitor_timer_t *restrict timer,
                                    uint64_t base_ms, uint64_t delta_ms) {
  return monitor_timer_arm_absolute(timer,
                                    monitor_deadline_add_ms(base_ms, delta_ms));
}

static bool monitor_timer_is_armed(const monitor_timer_t *restrict timer) {
  return timer != NULL && timer->armed;
}

static bool monitor_timer_elapsed(const monitor_timer_t *restrict timer,
                                  uint64_t now_ms) {
  return monitor_timer_is_armed(timer) && now_ms >= timer->deadline_ms;
}

static int monitor_deadline_timeout_ms(uint64_t now_ms, uint64_t deadline_ms) {
  if (deadline_ms <= now_ms) {
    return 0;
  }
  uint64_t remaining_ms = deadline_ms - now_ms;
  if (remaining_ms > (uint64_t)INT_MAX) {
    return INT_MAX;
  }
  return (int)remaining_ms;
}

static int monitor_timeout_min(int lhs, int rhs) {
  return rhs < lhs ? rhs : lhs;
}

static int monitor_timer_timeout_ms(const monitor_timer_t *restrict timer,
                                    uint64_t now_ms) {
  return monitor_timer_is_armed(timer)
             ? monitor_deadline_timeout_ms(now_ms, timer->deadline_ms)
             : -1;
}

static int monitor_timeout_accumulate(int timeout_ms,
                                      const monitor_timer_t *restrict timer,
                                      uint64_t now_ms) {
  int candidate_ms = monitor_timer_timeout_ms(timer, now_ms);
  if (candidate_ms < 0) {
    return timeout_ms;
  }

  return timeout_ms < 0 ? candidate_ms
                        : monitor_timeout_min(timeout_ms, candidate_ms);
}

static bool monitor_timer_step_interval(monitor_timer_t *restrict timer,
                                        uint64_t interval_ms, uint64_t now_ms) {
  if (timer == NULL || interval_ms == 0) {
    return false;
  }

  uint64_t base_ms =
      monitor_timer_is_armed(timer) ? timer->deadline_ms : now_ms;
  uint64_t next_deadline_ms = monitor_deadline_add_ms(base_ms, interval_ms);
  if (next_deadline_ms == UINT64_MAX || next_deadline_ms < now_ms) {
    next_deadline_ms = monitor_deadline_add_ms(now_ms, interval_ms);
  }

  return monitor_timer_arm_absolute(timer, next_deadline_ms);
}

static void monitor_state_init(monitor_state_t *restrict state, uint64_t now_ms,
                               uint64_t interval_ms,
                               uint64_t watchdog_interval_ms) {
  if (state == NULL) {
    return;
  }
  memset(state, 0, sizeof(*state));
  (void)monitor_timer_arm_absolute(&state->scheduler.next_ping_timer, now_ms);
  state->scheduler.interval_ms = interval_ms;
  state->watchdog.interval_ms = watchdog_interval_ms;
  if (watchdog_interval_ms > 0) {
    (void)monitor_timer_arm_after(&state->watchdog.next_notify_timer, now_ms,
                                  watchdog_interval_ms);
  }
}

static bool monitor_ping_arm(monitor_state_t *restrict state, uint64_t now_ms,
                             uint64_t timeout_ms, uint16_t expected_sequence) {
  if (state == NULL) {
    return false;
  }
  if (!monitor_timer_arm_after(&state->ping.reply_timer, now_ms, timeout_ms)) {
    return false;
  }
  state->ping.send_time_ms = now_ms;
  state->ping.expected_sequence = expected_sequence;
  return true;
}

static void monitor_ping_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }
  monitor_timer_clear(&state->ping.reply_timer);
  state->ping.send_time_ms = 0;
  state->ping.expected_sequence = 0;
}

static bool monitor_ping_waiting(const monitor_state_t *restrict state) {
  return state != NULL && monitor_timer_is_armed(&state->ping.reply_timer);
}

static bool monitor_ping_deadline_elapsed(const monitor_state_t *restrict state,
                                          uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_elapsed(&state->ping.reply_timer, now_ms);
}

static uint64_t
monitor_ping_send_time_ms(const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.send_time_ms : 0;
}

static uint16_t
monitor_ping_expected_sequence(const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.expected_sequence : 0;
}

static bool monitor_shutdown_arm(monitor_state_t *restrict state,
                                 uint64_t now_ms, uint64_t delay_ms) {
  if (state == NULL) {
    return false;
  }
  return monitor_timer_arm_after(&state->shutdown.countdown_timer, now_ms,
                                 delay_ms);
}

static void monitor_shutdown_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }
  monitor_timer_clear(&state->shutdown.countdown_timer);
}

static bool monitor_shutdown_pending(const monitor_state_t *restrict state) {
  return state != NULL &&
         monitor_timer_is_armed(&state->shutdown.countdown_timer);
}

static bool
monitor_shutdown_deadline_elapsed(const monitor_state_t *restrict state,
                                  uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_elapsed(&state->shutdown.countdown_timer, now_ms);
}

static bool monitor_scheduler_due(const monitor_state_t *restrict state,
                                  uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_elapsed(&state->scheduler.next_ping_timer, now_ms);
}

static bool monitor_scheduler_advance(monitor_state_t *restrict state,
                                      uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_step_interval(&state->scheduler.next_ping_timer,
                                     state->scheduler.interval_ms, now_ms);
}

static bool monitor_watchdog_due(const monitor_state_t *restrict state,
                                 uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_elapsed(&state->watchdog.next_notify_timer, now_ms);
}

static bool monitor_watchdog_mark_sent(monitor_state_t *restrict state,
                                       uint64_t now_ms) {
  return state != NULL &&
         monitor_timer_step_interval(&state->watchdog.next_notify_timer,
                                     state->watchdog.interval_ms, now_ms);
}

static int monitor_state_wait_timeout(const monitor_state_t *restrict state,
                                      uint64_t now_ms) {
  if (state == NULL) {
    return -1;
  }

  int timeout_ms =
      monitor_ping_waiting(state)
          ? monitor_timer_timeout_ms(&state->ping.reply_timer, now_ms)
          : monitor_timer_timeout_ms(&state->scheduler.next_ping_timer, now_ms);
  timeout_ms = monitor_timeout_accumulate(
      timeout_ms, &state->shutdown.countdown_timer, now_ms);
  timeout_ms = monitor_timeout_accumulate(
      timeout_ms, &state->watchdog.next_notify_timer, now_ms);
  return timeout_ms;
}

/* ---- Runtime services ---- */

static bool runtime_services_noop_ready(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static bool runtime_services_noop_status(void *backend_ctx,
                                         const char *status) {
  (void)backend_ctx;
  (void)status;
  return true;
}

static bool runtime_services_noop_stopping(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static bool runtime_services_noop_watchdog(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static uint64_t
runtime_services_noop_watchdog_interval_ms(const void *backend_ctx) {
  (void)backend_ctx;
  return 0;
}

static void runtime_services_noop_destroy(void *backend_ctx) {
  (void)backend_ctx;
}

static void runtime_services_set_null(runtime_services_t *restrict services) {
  if (services == NULL) {
    return;
  }

  services->backend_ctx = NULL;
  services->enabled = false;
  services->ready = runtime_services_noop_ready;
  services->status = runtime_services_noop_status;
  services->stopping = runtime_services_noop_stopping;
  services->watchdog = runtime_services_noop_watchdog;
  services->watchdog_interval_ms = runtime_services_noop_watchdog_interval_ms;
  services->destroy = runtime_services_noop_destroy;
}

/* Type-safe wrappers: avoid UB from casting incompatible function pointers. */
static bool runtime_services_systemd_ready(void *ctx) {
  return systemd_notifier_ready((systemd_notifier_t *)ctx);
}

static bool runtime_services_systemd_status(void *ctx, const char *status) {
  return systemd_notifier_status((systemd_notifier_t *)ctx, status);
}

static bool runtime_services_systemd_stopping(void *ctx) {
  return systemd_notifier_stopping((systemd_notifier_t *)ctx);
}

static bool runtime_services_systemd_watchdog(void *ctx) {
  return systemd_notifier_watchdog((systemd_notifier_t *)ctx);
}

static uint64_t
runtime_services_systemd_watchdog_interval_ms(const void *ctx) {
  return systemd_notifier_watchdog_interval_ms(
      (const systemd_notifier_t *)ctx);
}

static void runtime_services_systemd_destroy(void *ctx) {
  systemd_notifier_destroy((systemd_notifier_t *)ctx);
}

static void runtime_services_init(runtime_services_t *restrict services,
                                  systemd_notifier_t *restrict systemd,
                                  bool enable_systemd) {
  if (services == NULL) {
    return;
  }

  runtime_services_set_null(services);
  if (!enable_systemd || systemd == NULL) {
    return;
  }

  systemd_notifier_init(systemd);
  if (!systemd_notifier_is_enabled(systemd)) {
    return;
  }

  services->backend_ctx = systemd;
  services->enabled = true;
  services->ready = runtime_services_systemd_ready;
  services->status = runtime_services_systemd_status;
  services->stopping = runtime_services_systemd_stopping;
  services->watchdog = runtime_services_systemd_watchdog;
  services->watchdog_interval_ms = runtime_services_systemd_watchdog_interval_ms;
  services->destroy = runtime_services_systemd_destroy;
}

static void runtime_services_destroy(runtime_services_t *restrict services) {
  if (services == NULL) {
    return;
  }

  services->destroy(services->backend_ctx);
  runtime_services_set_null(services);
}

static bool
runtime_services_is_enabled(const runtime_services_t *restrict services) {
  return services != NULL && services->enabled;
}

static uint64_t runtime_services_watchdog_interval_ms(
    const runtime_services_t *restrict services) {
  if (services == NULL) {
    return 0;
  }

  return services->watchdog_interval_ms(services->backend_ctx);
}

static bool
runtime_services_notify_status(runtime_services_t *restrict services,
                               const char *restrict status) {
  if (services == NULL || status == NULL) {
    return false;
  }

  if (!runtime_services_is_enabled(services)) {
    return true;
  }

  return services->status(services->backend_ctx, status);
}

static bool
runtime_services_notify_ready(runtime_services_t *restrict services) {
  return services != NULL && services->ready(services->backend_ctx);
}

static bool
runtime_services_notify_watchdog(runtime_services_t *restrict services) {
  return services != NULL && services->watchdog(services->backend_ctx);
}

static bool
runtime_services_notify_stopping(runtime_services_t *restrict services) {
  return services != NULL && services->stopping(services->backend_ctx);
}

static bool monitor_notify_statusf(linkstay_ctx_t *restrict ctx,
                                   const char *restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool monitor_notify_statusf(linkstay_ctx_t *restrict ctx,
                                   const char *restrict fmt, ...) {
  if (ctx == NULL || fmt == NULL) {
    return false;
  }

  char status_msg[LINKSTAY_SYSTEMD_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, args);
  va_end(args);

  return runtime_services_notify_status(&ctx->services, status_msg);
}

/* ---- Shutdown FSM — static ---- */

__attribute__((format(printf, 2, 3))) static monitor_step_result_t
monitor_runtime_error(linkstay_ctx_t *restrict ctx, const char *restrict fmt,
                      ...);

static uint64_t shutdown_fsm_config_delay_ms(const config_t *restrict config) {
  if (config == NULL || config->delay_minutes <= 0) {
    return 0;
  }
  uint64_t delay_ms = 0;
  if (LINKSTAY_UNLIKELY(ckd_mul(&delay_ms, (uint64_t)config->delay_minutes,
                                LINKSTAY_MS_PER_MINUTE))) {
    return UINT64_MAX;
  }
  return delay_ms;
}

static bool shutdown_fsm_execute(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return false;
  }
  shutdown_result_t result = shutdown_trigger(&ctx->config, &ctx->logger);
  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
    return true;
  }
  if (result != SHUTDOWN_RESULT_TRIGGERED) {
    logger_error(&ctx->logger, "Shutdown command failed; continuing monitoring "
                               "with failure count preserved");
    return false;
  }
  logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
  return true;
}

static bool shutdown_fsm_cancel(linkstay_ctx_t *restrict ctx,
                                monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL || !monitor_shutdown_pending(state)) {
    return false;
  }
  monitor_shutdown_clear(state);
  logger_info(&ctx->logger,
              "Connectivity restored; cancelled pending shutdown countdown");
  (void)monitor_notify_statusf(
      ctx, "Recovery detected; shutdown countdown cancelled");
  return true;
}

static monitor_step_result_t
shutdown_fsm_handle_threshold(linkstay_ctx_t *restrict ctx,
                              monitor_state_t *restrict state,
                              uint64_t now_ms) {
  if (ctx == NULL || state == NULL ||
      ctx->consecutive_fails < ctx->config.fail_threshold) {
    return MONITOR_STEP_CONTINUE;
  }
  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    logger_warn(&ctx->logger, "Log-only mode: failure threshold reached, "
                              "continuing monitoring without shutdown");
    ctx->consecutive_fails = 0;
    return MONITOR_STEP_CONTINUE;
  }
  if (ctx->config.delay_minutes <= 0) {
    return shutdown_fsm_execute(ctx) ? MONITOR_STEP_STOP
                                     : MONITOR_STEP_CONTINUE;
  }
  if (monitor_shutdown_pending(state)) {
    return MONITOR_STEP_CONTINUE;
  }
  uint64_t delay_ms = shutdown_fsm_config_delay_ms(&ctx->config);
  if (delay_ms == 0 || delay_ms == UINT64_MAX) {
    return monitor_runtime_error(
        ctx, "Failed to compute delayed shutdown countdown duration");
  }
  if (!monitor_shutdown_arm(state, now_ms, delay_ms)) {
    return monitor_runtime_error(ctx,
                                 "Failed to compute shutdown countdown deadline");
  }
  log_shutdown_countdown(&ctx->logger, ctx->config.shutdown_mode,
                         ctx->config.delay_minutes);
  (void)monitor_notify_statusf(
      ctx, "%s countdown started: %d minutes",
      shutdown_mode_to_string(ctx->config.shutdown_mode),
      ctx->config.delay_minutes);
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t
shutdown_fsm_handle_tick(linkstay_ctx_t *restrict ctx,
                         monitor_state_t *restrict state, uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !monitor_shutdown_pending(state) ||
      !monitor_shutdown_deadline_elapsed(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  monitor_shutdown_clear(state);
  logger_warn(&ctx->logger, "%s countdown elapsed; executing shutdown now",
              shutdown_mode_to_string(ctx->config.shutdown_mode));
  (void)monitor_notify_statusf(
      ctx, "%s countdown elapsed; executing shutdown",
      shutdown_mode_to_string(ctx->config.shutdown_mode));
  return shutdown_fsm_execute(ctx) ? MONITOR_STEP_STOP : MONITOR_STEP_CONTINUE;
}

/* ---- Runtime helpers — static ---- */

static void handle_ping_success(linkstay_ctx_t *restrict ctx,
                                monitor_state_t *restrict state,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || state == NULL || result == NULL) {
    return;
  }
  ctx->consecutive_fails = 0;
  (void)shutdown_fsm_cancel(ctx, state);
  metrics_record_success(&ctx->metrics, result->latency_ms);
  logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
               ctx->config.target, result->latency_ms);
  (void)monitor_notify_statusf(
      ctx, "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
      ctx->metrics.successful_pings, ctx->metrics.total_pings,
      metrics_success_rate(&ctx->metrics), result->latency_ms);
}

static void handle_ping_failure(linkstay_ctx_t *restrict ctx,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || result == NULL) {
    return;
  }
  ctx->consecutive_fails++;
  int consecutive_failures = ctx->consecutive_fails;
  metrics_record_failure(&ctx->metrics);
  logger_warn(&ctx->logger, "Ping failed to %s: %s (consecutive failures: %d)",
              ctx->config.target, result->error_msg, consecutive_failures);
  (void)monitor_notify_statusf(
      ctx, "WARNING: %d consecutive failures, threshold is %d",
      consecutive_failures, ctx->config.fail_threshold);
}

__attribute__((format(printf, 2, 3))) static monitor_step_result_t
monitor_runtime_error(linkstay_ctx_t *restrict ctx, const char *restrict fmt,
                      ...) {
  if (ctx == NULL || fmt == NULL) {
    return MONITOR_STEP_ERROR;
  }
  char error_msg[LINKSTAY_LOG_BUFFER_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(error_msg, sizeof(error_msg), fmt, args);
  va_end(args);
  logger_error(&ctx->logger, "%s", error_msg);
  (void)monitor_notify_statusf(ctx, "ERROR: %s", error_msg);
  return MONITOR_STEP_ERROR;
}

static bool monitor_prepare_packet(linkstay_ctx_t *restrict ctx,
                                   size_t *restrict packet_len) {
  if (ctx == NULL || packet_len == NULL) {
    return false;
  }
  size_t header_len = (ctx->pinger.family == AF_INET6)
                          ? sizeof(struct icmp6_hdr)
                          : sizeof(struct icmphdr);
  *packet_len = LINKSTAY_PACKET_SIZE;
  if (*packet_len > sizeof(ctx->pinger.send_buf) || header_len > *packet_len) {
    return false;
  }
  uint8_t *payload = ctx->pinger.send_buf + header_len;
  size_t payload_len = *packet_len - header_len;
  for (size_t i = 0; i != payload_len; i++) {
    payload[i] = (uint8_t)(i & 0xFFU);
  }
  return true;
}

static void monitor_log_stats(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }
  const metrics_t *metrics = &ctx->metrics;
  if (metrics->successful_pings > 0) {
    logger_info(&ctx->logger,
                "Statistics: %" PRIu64 " total pings, %" PRIu64
                " successful, %" PRIu64
                " failed (%.2f%% success rate), latency min %.2fms / max "
                "%.2fms / avg %.2fms, uptime %" PRIu64 " seconds",
                metrics->total_pings, metrics->successful_pings,
                metrics->failed_pings, metrics_success_rate(metrics),
                metrics->min_latency, metrics->max_latency,
                metrics_avg_latency(metrics), metrics_uptime_seconds(metrics));
    return;
  }
  logger_info(&ctx->logger,
              "Statistics: %" PRIu64 " total pings, 0 successful, %" PRIu64
              " failed (0.00%% success rate), latency N/A, uptime %" PRIu64
              " seconds",
              metrics->total_pings, metrics->failed_pings,
              metrics_uptime_seconds(metrics));
}

static monitor_step_result_t
monitor_handle_ping_timeout(linkstay_ctx_t *restrict ctx,
                            monitor_state_t *restrict state, uint64_t now_ms) {
  if (ctx == NULL || state == NULL ||
      !monitor_ping_deadline_elapsed(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  ping_result_t timeout_result = {false, 0.0, "ICMP reply deadline exceeded"};
  handle_ping_failure(ctx, &timeout_result);
  monitor_ping_clear(state);
  return shutdown_fsm_handle_threshold(ctx, state, now_ms);
}

static monitor_step_result_t monitor_send_ping(linkstay_ctx_t *restrict ctx,
                                               monitor_state_t *restrict state,
                                               uint64_t now_ms,
                                               size_t packet_len) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }
  ping_result_t error_result = {false, -1.0, {0}};
  if (!icmp_pinger_send_echo(
          &ctx->pinger, &ctx->dest_addr, ctx->dest_addr_len, ctx->cached_pid,
          packet_len, error_result.error_msg, sizeof(error_result.error_msg))) {
    return monitor_runtime_error(ctx, "Failed to send ICMP echo: %s",
                                 error_result.error_msg);
  }
  if (!monitor_ping_arm(state, now_ms, (uint64_t)ctx->config.timeout_ms,
                        ctx->pinger.sequence)) {
    return monitor_runtime_error(ctx, "Failed to compute reply deadline");
  }
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t
monitor_drain_icmp_replies(linkstay_ctx_t *restrict ctx, uint64_t now_ms,
                           monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }
  ping_result_t reply = {0};
  for (size_t processed = 0; processed < LINKSTAY_MAX_REPLY_DRAIN_PER_TICK;
       processed++) {
    icmp_receive_status_t status = icmp_pinger_receive_reply(
        &ctx->pinger, &ctx->dest_addr, ctx->cached_pid,
        monitor_ping_expected_sequence(state), monitor_ping_send_time_ms(state),
        now_ms, &reply);
    if (status == ICMP_RECEIVE_NO_MORE) {
      return MONITOR_STEP_CONTINUE;
    }
    if (status == ICMP_RECEIVE_ERROR) {
      monitor_ping_clear(state);
      return monitor_runtime_error(ctx, "ICMP receive failed: %s",
                                   reply.error_msg);
    }
    if (status == ICMP_RECEIVE_MATCHED && monitor_ping_waiting(state)) {
      handle_ping_success(ctx, state, &reply);
      monitor_ping_clear(state);
      return MONITOR_STEP_CONTINUE;
    }
  }
  return MONITOR_STEP_CONTINUE;
}

/* ---- Reactor ---- */

typedef struct {
  int fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
} signal_channel_t;

/* Renamed from monitor_runtime_t to avoid confusion with the merged module. */
typedef struct {
  signal_channel_t signals;
  monitor_state_t state;
  struct pollfd fds[2];
  size_t packet_len;
  uint64_t now_ms;
} monitor_loop_t;

static uint64_t config_duration_ms(int value, uint64_t scale_ms) {
  if (value <= 0) {
    return 0;
  }
  uint64_t duration_ms = 0;
  if (LINKSTAY_UNLIKELY(ckd_mul(&duration_ms, (uint64_t)value, scale_ms))) {
    return UINT64_MAX;
  }
  return duration_ms;
}

static bool pollfd_has_error(short revents) {
  return (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

static bool monitor_refresh_time(uint64_t *restrict now_ms) {
  if (now_ms == NULL) {
    return false;
  }
  uint64_t refreshed_now_ms = get_monotonic_ms();
  if (refreshed_now_ms == UINT64_MAX) {
    return false;
  }
  *now_ms = refreshed_now_ms;
  return true;
}

static monitor_step_result_t
monitor_refresh_time_or_error(linkstay_ctx_t *restrict ctx,
                              uint64_t *restrict now_ms,
                              const char *restrict phase_name) {
  if (monitor_refresh_time(now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }

  if (ctx == NULL || phase_name == NULL) {
    return MONITOR_STEP_ERROR;
  }

  return monitor_runtime_error(ctx,
                               "Failed to refresh monotonic clock during %s",
                               phase_name);
}

static bool signal_channel_init(signal_channel_t *restrict channel,
                                const logger_t *restrict logger) {
  if (channel == NULL || logger == NULL) {
    return false;
  }
  memset(channel, 0, sizeof(*channel));
  channel->fd = -1;
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &mask, &channel->previous_mask) < 0) {
    logger_error(logger, "sigprocmask failed: %s", strerror(errno));
    return false;
  }
  channel->previous_mask_valid = true;
  channel->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (channel->fd < 0) {
    logger_error(logger, "signalfd failed: %s", strerror(errno));
    (void)sigprocmask(SIG_SETMASK, &channel->previous_mask, NULL);
    channel->previous_mask_valid = false;
    return false;
  }
  return true;
}

static void signal_channel_destroy(signal_channel_t *restrict channel,
                                   const logger_t *restrict logger) {
  if (channel == NULL) {
    return;
  }
  if (channel->fd >= 0) {
    close(channel->fd);
    channel->fd = -1;
  }
  if (channel->previous_mask_valid &&
      sigprocmask(SIG_SETMASK, &channel->previous_mask, NULL) < 0 &&
      logger != NULL) {
    logger_error(logger, "sigprocmask restore failed: %s", strerror(errno));
  }
  channel->previous_mask_valid = false;
}

static void monitor_handle_signal(linkstay_ctx_t *restrict ctx,
                                  const signal_channel_t *restrict signals) {
  if (ctx == NULL || signals == NULL || signals->fd < 0) {
    return;
  }
  struct signalfd_siginfo signal_info;
  ssize_t bytes = read(signals->fd, &signal_info, sizeof(signal_info));
  if (bytes != (ssize_t)sizeof(signal_info)) {
    return;
  }
  if (signal_info.ssi_signo == SIGINT || signal_info.ssi_signo == SIGTERM) {
    ctx->stop_flag = 1;
    return;
  }
  if (signal_info.ssi_signo == SIGUSR1) {
    monitor_log_stats(ctx);
  }
}

static bool monitor_notify_ready(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL || !runtime_services_is_enabled(&ctx->services)) {
    return true;
  }
  if (runtime_services_notify_ready(&ctx->services)) {
    return true;
  }
  logger_warn(&ctx->logger, "Failed to send systemd READY notification");
  return false;
}

static monitor_step_result_t
monitor_handle_watchdog(linkstay_ctx_t *restrict ctx,
                        monitor_state_t *restrict state, uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !monitor_watchdog_due(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  bool watchdog_sent = runtime_services_notify_watchdog(&ctx->services);
  if (!monitor_watchdog_mark_sent(state, now_ms)) {
    return monitor_runtime_error(
        ctx, "Failed to schedule next systemd watchdog deadline");
  }

  if (!watchdog_sent) {
    logger_warn(&ctx->logger, "Failed to send systemd WATCHDOG notification");
  }
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t
monitor_handle_scheduler(linkstay_ctx_t *restrict ctx,
                         monitor_state_t *restrict state, uint64_t now_ms,
                         size_t packet_len) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }
  if (monitor_ping_waiting(state) || !monitor_scheduler_due(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  monitor_step_result_t send_result =
      monitor_send_ping(ctx, state, now_ms, packet_len);
  if (send_result != MONITOR_STEP_CONTINUE) {
    return send_result;
  }
  if (!monitor_scheduler_advance(state, now_ms)) {
    logger_error(&ctx->logger, "Failed to compute next ping deadline");
    return MONITOR_STEP_ERROR;
  }
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t monitor_handle_poll_events(
    linkstay_ctx_t *restrict ctx, const signal_channel_t *restrict signals,
    monitor_state_t *restrict state, struct pollfd fds[static 2],
    uint64_t *restrict now_ms) {
  if (ctx == NULL || signals == NULL || state == NULL || now_ms == NULL) {
    return MONITOR_STEP_ERROR;
  }
  int wait_timeout_ms = monitor_state_wait_timeout(state, *now_ms);
  int poll_result = poll(fds, 2, wait_timeout_ms);
  if (poll_result < 0 && errno != EINTR) {
    logger_error(&ctx->logger, "poll error: %s", strerror(errno));
    return MONITOR_STEP_ERROR;
  }
  monitor_step_result_t refresh_result =
      monitor_refresh_time_or_error(ctx, now_ms, "poll handling");
  if (refresh_result != MONITOR_STEP_CONTINUE) {
    return refresh_result;
  }
  if (pollfd_has_error(fds[0].revents)) {
    logger_error(&ctx->logger, "Signal fd entered error state");
    return MONITOR_STEP_ERROR;
  }
  if (pollfd_has_error(fds[1].revents)) {
    logger_error(&ctx->logger, "ICMP socket entered error state");
    return MONITOR_STEP_ERROR;
  }
  if ((fds[0].revents & POLLIN) != 0) {
    monitor_handle_signal(ctx, signals);
  }
  if ((fds[1].revents & POLLIN) != 0) {
    monitor_step_result_t receive_result =
        monitor_drain_icmp_replies(ctx, *now_ms, state);
    if (receive_result != MONITOR_STEP_CONTINUE) {
      return receive_result;
    }
  }
  fds[0].revents = 0;
  fds[1].revents = 0;
  return MONITOR_STEP_CONTINUE;
}

static bool monitor_loop_init(linkstay_ctx_t *restrict ctx,
                              monitor_loop_t *restrict loop) {
  if (ctx == NULL || loop == NULL) {
    return false;
  }
  memset(loop, 0, sizeof(*loop));
  loop->signals.fd = -1;
  if (!signal_channel_init(&loop->signals, &ctx->logger)) {
    return false;
  }
  if (!monitor_prepare_packet(ctx, &loop->packet_len)) {
    logger_error(&ctx->logger, "Failed to prepare ICMP packet buffer");
    signal_channel_destroy(&loop->signals, &ctx->logger);
    return false;
  }
  uint64_t interval_ms =
      config_duration_ms(ctx->config.interval_sec, LINKSTAY_MS_PER_SEC);
  if (!monitor_refresh_time(&loop->now_ms) || interval_ms == 0 ||
      interval_ms == UINT64_MAX) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    signal_channel_destroy(&loop->signals, &ctx->logger);
    return false;
  }
  monitor_state_init(&loop->state, loop->now_ms, interval_ms,
                     runtime_services_watchdog_interval_ms(&ctx->services));
  loop->fds[0] = (struct pollfd){
      .fd = loop->signals.fd,
      .events = POLLIN,
      .revents = 0,
  };
  loop->fds[1] = (struct pollfd){
      .fd = ctx->pinger.sockfd,
      .events = POLLIN,
      .revents = 0,
  };
  return true;
}

static void monitor_loop_destroy(linkstay_ctx_t *restrict ctx,
                                 monitor_loop_t *restrict loop) {
  if (loop == NULL) {
    return;
  }
  signal_channel_destroy(&loop->signals, ctx != NULL ? &ctx->logger : NULL);
}

static monitor_step_result_t
monitor_run_due_work(linkstay_ctx_t *restrict ctx,
                     monitor_loop_t *restrict loop) {
  if (ctx == NULL || loop == NULL) {
    return MONITOR_STEP_ERROR;
  }
  monitor_step_result_t step_result =
      shutdown_fsm_handle_tick(ctx, &loop->state, loop->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }
  step_result = monitor_handle_watchdog(ctx, &loop->state, loop->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }
  step_result = monitor_handle_ping_timeout(ctx, &loop->state, loop->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }
  return monitor_handle_scheduler(ctx, &loop->state, loop->now_ms,
                                  loop->packet_len);
}

static void monitor_log_startup(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }
  logger_info(&ctx->logger, "Starting LinkStay for target %s, every %ds",
              ctx->config.target, ctx->config.interval_sec);
  (void)monitor_notify_ready(ctx);
  (void)monitor_notify_statusf(ctx, "Monitoring %s", ctx->config.target);
}

static void monitor_log_shutdown(linkstay_ctx_t *restrict ctx, int exit_code) {
  if (ctx == NULL) {
    return;
  }
  if (ctx->stop_flag) {
    logger_info(&ctx->logger,
                "Received shutdown signal, stopping gracefully...");
    if (runtime_services_is_enabled(&ctx->services)) {
      (void)runtime_services_notify_stopping(&ctx->services);
    }
  }
  monitor_log_stats(ctx);
  if (exit_code == LINKSTAY_EXIT_SUCCESS) {
    logger_info(&ctx->logger, "LinkStay monitor stopped");
  }
}

/* ---- Public API ---- */

bool linkstay_ctx_init(linkstay_ctx_t *restrict ctx,
                       const config_t *restrict config,
                       char *restrict error_msg, size_t error_size) {
  if (ctx == NULL || config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->config = *config;
  ctx->cached_pid = (uint16_t)(getpid() & 0xFFFF);
  if (ctx->cached_pid == 0) {
    ctx->cached_pid = 1;
  }
  logger_init(&ctx->logger, ctx->config.log_level,
              config_log_timestamps_enabled(&ctx->config));
  if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
    config_print(&ctx->config, &ctx->logger);
  }
  ctx->dest_addr_len = sizeof(ctx->dest_addr);
  if (!resolve_target(ctx->config.target, &ctx->dest_addr, &ctx->dest_addr_len,
                      error_msg, error_size)) {
    return false;
  }
  int family = ((const struct sockaddr *)&ctx->dest_addr)->sa_family;
  if (!icmp_pinger_init(&ctx->pinger, family, error_msg, error_size)) {
    return false;
  }
  metrics_init(&ctx->metrics);
  runtime_services_init(&ctx->services, &ctx->systemd,
                        ctx->config.enable_systemd);
  if (!runtime_services_is_enabled(&ctx->services)) {
    logger_debug(&ctx->logger, "systemd not detected, integration disabled");
    return true;
  }
  uint64_t watchdog_interval_ms =
      runtime_services_watchdog_interval_ms(&ctx->services);
  logger_debug(&ctx->logger, "systemd integration enabled");
  if (watchdog_interval_ms > 0) {
    logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                 watchdog_interval_ms);
  }
  return true;
}

void linkstay_ctx_destroy(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }
  runtime_services_destroy(&ctx->services);
  icmp_pinger_destroy(&ctx->pinger);
  memset(ctx, 0, sizeof(*ctx));
}

int linkstay_reactor_run(linkstay_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return LINKSTAY_EXIT_FAILURE;
  }
  monitor_loop_t loop;
  if (!monitor_loop_init(ctx, &loop)) {
    return LINKSTAY_EXIT_FAILURE;
  }
  int exit_code = LINKSTAY_EXIT_SUCCESS;
  monitor_log_startup(ctx);
  while (!ctx->stop_flag) {
    monitor_step_result_t refresh_result =
        monitor_refresh_time_or_error(ctx, &loop.now_ms, "reactor loop");
    if (refresh_result == MONITOR_STEP_ERROR) {
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }
    monitor_step_result_t step_result = monitor_run_due_work(ctx, &loop);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }
    step_result = monitor_handle_poll_events(ctx, &loop.signals, &loop.state,
                                             loop.fds, &loop.now_ms);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = LINKSTAY_EXIT_FAILURE;
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }
  }
  monitor_log_shutdown(ctx, exit_code);
  monitor_loop_destroy(ctx, &loop);
  return exit_code;
}
