#ifndef LINKSTAY_APP_H
#define LINKSTAY_APP_H

/*
 * app.h — application assembly layer.
 *
 * Wires resolved options, the ICMP transport, and the notifier into the event
 * loop and owns the probe state machine. Statistics, threshold action, and
 * systemd notify integration are implementation details of app.c; their
 * types appear here only because ls_app_t embeds them.
 */

#include "base.h"
#include "icmp.h"
#include "loop.h"
#include "opts.h"

/* ---- Statistics value type ---- */

typedef struct {
  uint64_t total;
  uint64_t ok;
  uint64_t failed;
  uint64_t latency_sum_us;
  uint64_t latency_min_us;
  uint64_t latency_max_us;
  uint64_t started_at_ms;
} ls_stats_t;

/* ---- systemd notify state ---- */

#define LS_NOTIFY_STATUS_SIZE 240U

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LS_NOTIFY_STATUS_SIZE];
} ls_notify_t;

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
