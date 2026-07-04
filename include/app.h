#ifndef LINKSTAY_APP_H
#define LINKSTAY_APP_H

/*
 * app.h — application assembly.
 *
 * Wires the resolved options, ICMP transport, statistics, notifier, and
 * threshold action into the event loop. Owns the explicit probe state
 * machine (IDLE ↔ AWAIT_REPLY) and the consecutive-failure counter.
 */

#include "base.h"
#include "icmp.h"
#include "log.h"
#include "loop.h"
#include "notify.h"
#include "opts.h"
#include "stats.h"

typedef enum {
  LS_PROBE_IDLE = 0,    /* no echo request outstanding */
  LS_PROBE_AWAIT_REPLY, /* echo sent, waiting for reply or deadline */
} ls_probe_state_t;

typedef struct {
  ls_opts_t opts;
  ls_log_t log;
  ls_stats_t stats;
  ls_icmp_t icmp;
  ls_notify_t notify;

  struct sockaddr_storage dest;
  socklen_t dest_len;
  uint16_t identifier; /* pid & 0xFFFF, cached off the hot path */

  ls_loop_t loop;
  ls_timer_t *ping_timer;     /* periodic echo scheduler */
  ls_timer_t *reply_timer;    /* deadline of the outstanding probe */
  ls_timer_t *watchdog_timer; /* systemd watchdog heartbeat */

  ls_probe_state_t probe_state;
  uint64_t probe_sent_ns; /* fine-grained clock, for sub-ms latency */
  uint16_t probe_sequence;
  int consecutive_fails;

  uint64_t interval_ms;
  uint64_t watchdog_interval_ms;
  bool watchdog_last_failed; /* throttles repeated send-failure warnings */
  bool signal_stop;          /* loop exit caused by SIGINT/SIGTERM */
} ls_app_t;

[[nodiscard]] bool ls_app_init(ls_app_t *restrict app,
                               const ls_opts_t *restrict opts,
                               ls_err_t *restrict err);
int ls_app_run(ls_app_t *restrict app);
void ls_app_destroy(ls_app_t *restrict app);

#endif /* LINKSTAY_APP_H */
