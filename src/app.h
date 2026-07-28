#ifndef LINKSTAY_APP_H
#define LINKSTAY_APP_H

/*
 * app.h — application assembly, statistics, threshold action, and systemd
 * notify integration.
 *
 * Wires the resolved options, ICMP transport, and notifier into the event
 * loop. Owns the explicit probe state machine (IDLE ↔ AWAIT_REPLY) and the
 * consecutive-failure counter. Also bundles the statistics value type,
 * threshold action backend, and sd_notify integration.
 */

#include "base.h"
#include "icmp.h"
#include "loop.h"
#include "opts.h"

/* ---- Statistics value type ---- */

#define LS_STATS_NO_SAMPLE UINT64_MAX

typedef struct {
  uint64_t total;
  uint64_t ok;
  uint64_t failed;
  uint64_t latency_sum_us;
  uint64_t latency_min_us;
  uint64_t latency_max_us;
  uint64_t started_at_ms;
} ls_stats_t;

void ls_stats_init(ls_stats_t *stats);
void ls_stats_add_ok(ls_stats_t *stats, double latency_ms);
void ls_stats_add_fail(ls_stats_t *stats);

[[nodiscard]] double ls_stats_avg_latency(const ls_stats_t *stats);
[[nodiscard]] double ls_stats_latency_min_ms(const ls_stats_t *stats);
[[nodiscard]] double ls_stats_latency_max_ms(const ls_stats_t *stats);
[[nodiscard]] double ls_stats_success_rate(const ls_stats_t *stats);
[[nodiscard]] uint64_t ls_stats_uptime_sec(const ls_stats_t *stats);

/* ---- Threshold action ---- */

typedef enum {
  LS_ACTION_SIMULATED = 0,
  LS_ACTION_TRIGGERED = 1,
  LS_ACTION_FAILED = 2,
} ls_action_result_t;

ls_action_result_t ls_action_shutdown(bool poweroff,
                                      const ls_log_t *restrict log);

/* ---- systemd notify integration ---- */

#define LS_NOTIFY_STATUS_SIZE 240U
#define LS_NOTIFY_DEDUP_WINDOW_MS UINT64_C(2000)

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LS_NOTIFY_STATUS_SIZE];
} ls_notify_t;

void ls_notify_init(ls_notify_t *restrict notify);
void ls_notify_destroy(ls_notify_t *restrict notify);
[[nodiscard]] bool ls_notify_enabled(const ls_notify_t *restrict notify);

bool ls_notify_ready(ls_notify_t *restrict notify);
[[gnu::format(printf, 2, 3)]] bool
ls_notify_statusf(ls_notify_t *restrict notify, const char *restrict fmt, ...);
bool ls_notify_stopping(ls_notify_t *restrict notify);
bool ls_notify_watchdog(ls_notify_t *restrict notify);
[[nodiscard]] uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify);

/* ---- Application state ---- */

typedef enum {
  LS_PROBE_IDLE = 0,
  LS_PROBE_AWAIT_REPLY,
} ls_probe_state_t;

typedef struct {
  ls_opts_t opts;
  ls_log_t log;
  ls_stats_t stats;
  ls_icmp_t icmp;
  ls_notify_t notify;

  struct sockaddr_storage dest;
  socklen_t dest_len;
  uint16_t identifier;

  ls_loop_t loop;
  ls_timer_t *ping_timer;
  ls_timer_t *reply_timer;
  ls_timer_t *watchdog_timer;

  ls_probe_state_t probe_state;
  uint64_t probe_sent_ns;
  uint16_t probe_sequence;
  int consecutive_fails;

  uint64_t interval_ms;
  uint64_t watchdog_interval_ms;
  bool watchdog_last_failed;
  bool signal_stop;
} ls_app_t;

[[nodiscard]] bool ls_app_init(ls_app_t *restrict app,
                               const ls_opts_t *restrict opts,
                               ls_err_t *restrict err);
int ls_app_run(ls_app_t *restrict app);
void ls_app_destroy(ls_app_t *restrict app);

#endif /* LINKSTAY_APP_H */
