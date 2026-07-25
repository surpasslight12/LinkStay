#include "app.h"

#include "action.h"

#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define LS_PACKET_SIZE 64U
#define LS_MAX_REPLY_DRAIN_PER_WAKEUP 32U

/* Nanoseconds-to-milliseconds conversion (kept as double for latency). */
#define LS_NS_PER_MS_F 1000000.0

/* Safety-warning thresholds. */
#define LS_WARN_THRESHOLD_MIN_WITH_POWEROFF 3
#define LS_WARN_TIMEOUT_AGGRESSIVE_MS 500
#define LS_WARN_TIMEOUT_INTERVAL_RATIO_PCT 80
#define LS_WARN_INTERVAL_LONG_SEC 3600
#define LS_WARN_THRESHOLD_HIGH 100

/* ---- Error path ---- */

/* Logs, mirrors the message to the systemd status, and stops the loop. */
static void app_fail(ls_app_t *app, const char *what, const ls_err_t *err) {
  ls_error(&app->log, "%s: %s", what, err->msg);
  ls_notify_statusf(&app->notify, "Error: %s: %s", what, err->msg);
  ls_loop_stop(&app->loop, LS_EXIT_FAILURE);
}

/* ---- Statistics reporting ---- */

static void log_statistics(ls_app_t *app) {
  const ls_stats_t *s = &app->stats;
  if (s->ok > 0) {
    ls_info(&app->log,
            "Statistics: uptime %" PRIu64 "s | %" PRIu64 " pings, %" PRIu64
            " OK, %" PRIu64
            " failed (%.2f%% success) | latency min %.2fms, avg %.2fms, "
            "max %.2fms",
            ls_stats_uptime_sec(s), s->total, s->ok, s->failed,
            ls_stats_success_rate(s), ls_stats_latency_min_ms(s),
            ls_stats_avg_latency(s), ls_stats_latency_max_ms(s));
  } else {
    ls_info(&app->log,
            "Statistics: uptime %" PRIu64 "s | %" PRIu64
            " pings, 0 OK, %" PRIu64 " failed (0.00%% success) | latency N/A",
            ls_stats_uptime_sec(s), s->total, s->failed);
  }
}

/* ---- Probe state machine ---- */

static void probe_reset(ls_app_t *app) {
  app->probe_state = LS_PROBE_IDLE;
  app->probe_sent_ns = 0;
  app->probe_sequence = 0;
  ls_timer_disarm(app->reply_timer);
}

static void on_probe_ok(ls_app_t *app, uint16_t sequence, double latency_ms) {
  app->consecutive_fails = 0;
  ls_stats_add_ok(&app->stats, latency_ms);
  ls_debug(&app->log,
           "Reply from %s: seq=%u time=%.2fms (avg %.2fms, %" PRIu64
           "/%" PRIu64 " OK, %.1f%% success)",
           app->opts.target, (unsigned)sequence, latency_ms,
           ls_stats_avg_latency(&app->stats), app->stats.ok, app->stats.total,
           ls_stats_success_rate(&app->stats));
  ls_notify_statusf(&app->notify,
                    "Online: %s up %" PRIu64 "s, last %.2fms, avg %.2fms, "
                    "%" PRIu64 "/%" PRIu64 " OK (%.1f%%)",
                    app->opts.target, ls_stats_uptime_sec(&app->stats),
                    latency_ms, ls_stats_avg_latency(&app->stats),
                    app->stats.ok, app->stats.total,
                    ls_stats_success_rate(&app->stats));
}

static void on_probe_timeout(ls_app_t *app) {
  app->consecutive_fails++;
  ls_stats_add_fail(&app->stats);
  ls_warn(&app->log,
          "No reply from %s: ICMP reply deadline exceeded (failure %d of %d)",
          app->opts.target, app->consecutive_fails, app->opts.fail_threshold);
  ls_notify_statusf(&app->notify, "Unreachable: %s, %d/%d consecutive failures",
                    app->opts.target, app->consecutive_fails,
                    app->opts.fail_threshold);
}

/* Runs the threshold action once enough consecutive failures accumulate. */
static void check_threshold(ls_app_t *app) {
  if (app->consecutive_fails < app->opts.fail_threshold) {
    return;
  }

  ls_action_result_t result =
      ls_action_shutdown(app->opts.poweroff, &app->log);
  if (!app->opts.poweroff) {
    ls_info(&app->log, "Dry-run complete: simulated shutdown reached, "
                       "exiting monitor loop");
    ls_loop_stop(&app->loop, LS_EXIT_SUCCESS);
    return;
  }
  if (result != LS_ACTION_TRIGGERED) {
    ls_error(&app->log, "Shutdown command failed; continuing monitoring with "
                        "failure count preserved");
    return;
  }
  ls_info(&app->log, "Shutdown triggered, exiting monitor loop");
  ls_loop_stop(&app->loop, LS_EXIT_SUCCESS);
}

/* ---- Shared reply-matching helper ----

 * Drains the ICMP socket non-blocking and processes the first matching
 * reply for the outstanding probe. Returns true when a matching reply was
 * accepted (probe state already reset and statistics recorded), false when
 * the socket is exhausted without a match.
 *
 * On receive errors this calls app_fail (which stops the loop), so the
 * caller must check app->loop.stopping after a false return. */
static bool drain_and_match_reply(ls_app_t *app) {
  ls_err_t err = {};
  for (size_t i = 0; i < LS_MAX_REPLY_DRAIN_PER_WAKEUP; i++) {
    ls_icmp_recv_status_t status =
        ls_icmp_recv(&app->icmp, &app->dest, app->identifier,
                     app->probe_sequence, &err);
    if (status == LS_ICMP_RECV_NO_MORE) {
      return false;
    }
    if (status == LS_ICMP_RECV_ERROR) {
      probe_reset(app);
      app_fail(app, "ICMP receive failed", &err);
      return false;
    }
    if (status == LS_ICMP_RECV_MATCHED &&
        app->probe_state == LS_PROBE_AWAIT_REPLY) {
      /* Sub-ms latency from the fine-grained clock; guard clock failure
       * and impossible skew by clamping to zero. */
      uint64_t now_ns = ls_now_ns();
      double latency_ms =
          (now_ns != UINT64_MAX && app->probe_sent_ns != UINT64_MAX &&
           now_ns >= app->probe_sent_ns)
              ? (double)(now_ns - app->probe_sent_ns) / LS_NS_PER_MS_F
              : 0.0;
      uint16_t sequence = app->probe_sequence;
      probe_reset(app);
      on_probe_ok(app, sequence, latency_ms);
      return true;
    }
  }
  return false;
}

/* ---- Loop callbacks ---- */

static void on_ping_timer(ls_loop_t *loop, void *userdata) {
  ls_app_t *app = userdata;
  uint64_t now_ms = ls_loop_now(loop);

  /* Always keep the periodic schedule, but never overlap probes. With
   * timeout + margin < interval (validated in opts.c), an outstanding
   * probe here is unusual. */
  ls_timer_step(app->ping_timer, app->interval_ms, now_ms);
  if (app->probe_state == LS_PROBE_AWAIT_REPLY) {
    return;
  }

  ls_err_t err = {};
  if (!ls_icmp_send_echo(&app->icmp, &app->dest, app->dest_len,
                         app->identifier, LS_PACKET_SIZE, &err)) {
    app_fail(app, "Failed to send ICMP echo", &err);
    return;
  }
  app->probe_state = LS_PROBE_AWAIT_REPLY;
  app->probe_sent_ns = ls_now_ns();
  app->probe_sequence = app->icmp.sequence;
  ls_timer_arm_after(app->reply_timer, now_ms,
                     (uint64_t)app->opts.timeout_ms);
}

static void on_reply_timer(ls_loop_t *loop, void *userdata) {
  (void)loop;
  ls_app_t *app = userdata;

  /* Defensive: bail if the probe was already resolved by another path
   * (e.g. a late poll wakeup that raced with this timer firing). */
  if (app->probe_state != LS_PROBE_AWAIT_REPLY) {
    return;
  }

  /* Last-chance non-blocking drain: the reply may have landed in the
   * socket buffer between the most recent poll wakeup and this timer
   * firing. If we can consume it now we avoid a spurious failure. */
  if (drain_and_match_reply(app)) {
    return; /* Reply arrived just in time — success already recorded */
  }
  if (app->loop.stopping) {
    return; /* drain_and_match_reply triggered a fatal error */
  }

  /* Genuine timeout — no matching reply was waiting. */
  probe_reset(app);
  on_probe_timeout(app);
  check_threshold(app);
}

static void on_watchdog_timer(ls_loop_t *loop, void *userdata) {
  ls_app_t *app = userdata;
  bool sent = ls_notify_watchdog(&app->notify);
  ls_timer_step(app->watchdog_timer, app->watchdog_interval_ms,
                ls_loop_now(loop));
  /* Log only on the failure/recovery edge so a persistently broken notify
   * socket does not spam the log every watchdog interval. */
  if (!sent && !app->watchdog_last_failed) {
    ls_warn(&app->log, "Failed to send systemd WATCHDOG notification");
  } else if (sent && app->watchdog_last_failed) {
    ls_info(&app->log, "systemd WATCHDOG notification recovered");
  }
  app->watchdog_last_failed = !sent;
}

static void on_icmp_readable(ls_loop_t *loop, int fd, void *userdata) {
  (void)loop;
  (void)fd;
  ls_app_t *app = userdata;
  drain_and_match_reply(app);
}

static void on_signal(ls_loop_t *loop, uint32_t signo, void *userdata) {
  ls_app_t *app = userdata;
  if (signo == SIGINT || signo == SIGTERM) {
    app->signal_stop = true;
    ls_loop_stop(loop, LS_EXIT_SUCCESS);
  } else if (signo == SIGUSR1) {
    log_statistics(app);
  }
}

/* ---- Startup / shutdown banners ---- */

static void log_startup(ls_app_t *app) {
  int detection_sec = app->opts.fail_threshold * app->opts.interval_sec;
  ls_info(&app->log,
          LS_PROGRAM_NAME " %s monitoring %s | interval %ds, timeout %dms, " \
          "threshold %d (detection ~%ds), poweroff %s",
          LS_VERSION, app->opts.target, app->opts.interval_sec,
          app->opts.timeout_ms, app->opts.fail_threshold, detection_sec,
          app->opts.poweroff ? "true" : "false");
  if (ls_notify_enabled(&app->notify) && !ls_notify_ready(&app->notify)) {
    ls_warn(&app->log, "Failed to send systemd READY notification");
  }
  ls_notify_statusf(&app->notify, "Monitoring %s every %ds (poweroff %s)",
                    app->opts.target, app->opts.interval_sec,
                    app->opts.poweroff ? "true" : "false");
}

static void log_shutdown(ls_app_t *app, int exit_code) {
  if (app->signal_stop) {
    ls_info(&app->log, "Shutdown signal received, stopping gracefully");
  }
  /* Notify systemd we are stopping on every exit path (signal, threshold
   * dry-run, or real poweroff) so the service manager always sees a clean
   * transition. */
  if (ls_notify_enabled(&app->notify)) {
    (void)ls_notify_stopping(&app->notify);
  }
  log_statistics(app);
  if (exit_code == LS_EXIT_SUCCESS) {
    ls_info(&app->log, LS_PROGRAM_NAME " monitor stopped");
  }
}

/* ---- Public API ---- */

bool ls_app_init(ls_app_t *restrict app, const ls_opts_t *restrict opts,
                 ls_err_t *restrict err) {
  if (app == nullptr || opts == nullptr) {
    return ls_err_set(err, "Invalid application init arguments");
  }

  *app = (ls_app_t){
      .opts = *opts,
      .identifier = (uint16_t)(getpid() & 0xFFFF),
  };
  if (app->identifier == 0) {
    app->identifier = 1;
  }
  /* Keep destroy safe even if init bails before these are opened. */
  app->icmp.sockfd = -1;
  app->notify.sockfd = -1;

  ls_log_init(&app->log, app->opts.log_level, ls_opts_timestamps(&app->opts));
  if (app->opts.log_level == LS_LOG_DEBUG) {
    ls_opts_dump(&app->opts, &app->log);
  }

  app->dest_len = sizeof(app->dest);
  if (!ls_icmp_resolve(app->opts.target, &app->dest, &app->dest_len, err)) {
    return false;
  }

  int family = ((const struct sockaddr *)&app->dest)->sa_family;
  if (!ls_icmp_open(&app->icmp, family, err)) {
    return false;
  }
  if (app->icmp.bpf_errno != 0) {
    ls_warn(&app->log,
            "ICMP BPF filter unavailable for %s traffic: %s; continuing "
            "without kernel-side packet filtering",
            family == AF_INET6 ? "IPv6" : "IPv4",
            strerror(app->icmp.bpf_errno));
  } else if (app->icmp.bpf_attached) {
    ls_debug(&app->log, "ICMP BPF filter active for %s traffic",
             family == AF_INET6 ? "IPv6" : "IPv4");
  }

  ls_stats_init(&app->stats);

  if (app->opts.systemd) {
    ls_notify_init(&app->notify);
  }
  app->interval_ms = (uint64_t)app->opts.interval_sec * LS_MS_PER_SEC;
  app->watchdog_interval_ms = ls_notify_watchdog_interval_ms(&app->notify);

  if (!ls_notify_enabled(&app->notify)) {
    ls_debug(&app->log, "systemd integration inactive (NOTIFY_SOCKET not set)");
  } else if (app->watchdog_interval_ms > 0) {
    ls_debug(&app->log,
             "systemd integration active, watchdog ping every %" PRIu64 "ms",
             app->watchdog_interval_ms);
  } else {
    ls_debug(&app->log, "systemd integration active, watchdog disabled");
  }

  /* ---- Parameter safety warnings ---- */

  if (app->opts.poweroff &&
      app->opts.fail_threshold < LS_WARN_THRESHOLD_MIN_WITH_POWEROFF) {
    ls_warn(&app->log,
            "Poweroff is enabled but threshold is only %d consecutive "
            "failures — a single network blip could trigger a shutdown",
            app->opts.fail_threshold);
  }
  if (app->opts.timeout_ms < LS_WARN_TIMEOUT_AGGRESSIVE_MS) {
    ls_warn(&app->log,
            "Timeout is very aggressive (%d ms); false positives are likely "
            "on high-latency WAN or satellite links",
            app->opts.timeout_ms);
  }
  if ((uint64_t)app->opts.timeout_ms * 100 >
      app->interval_ms * LS_WARN_TIMEOUT_INTERVAL_RATIO_PCT) {
    ls_warn(&app->log,
            "Timeout (%d ms) is close to the interval (%" PRIu64
            " ms); little margin remains for late replies",
            app->opts.timeout_ms, app->interval_ms);
  }
  if (app->opts.interval_sec > LS_WARN_INTERVAL_LONG_SEC) {
    ls_info(&app->log,
            "Interval is very long (%d s); network outages may take hours "
            "to detect",
            app->opts.interval_sec);
  }
  if (app->opts.fail_threshold > LS_WARN_THRESHOLD_HIGH) {
    ls_info(&app->log,
            "Failure threshold is very high (%d); shutdown would require "
            "%d consecutive failures",
            app->opts.fail_threshold, app->opts.fail_threshold);
  }

  return true;
}

void ls_app_destroy(ls_app_t *restrict app) {
  if (app == nullptr) {
    return;
  }
  ls_notify_destroy(&app->notify);
  ls_icmp_close(&app->icmp);
  *app = (ls_app_t){};
}

int ls_app_run(ls_app_t *restrict app) {
  if (app == nullptr) {
    return LS_EXIT_FAILURE;
  }

  ls_err_t err = {};
  if (!ls_loop_init(&app->loop, &app->log, &err)) {
    ls_error(&app->log, "%s", err.msg);
    return LS_EXIT_FAILURE;
  }

  static const int SIGNALS[] = {SIGINT, SIGTERM, SIGUSR1};
  bool wired =
      ls_loop_watch_signals(&app->loop, SIGNALS, LS_ARRAY_LEN(SIGNALS),
                            on_signal, app, &err) &&
      ls_loop_add_fd(&app->loop, app->icmp.sockfd, on_icmp_readable, app,
                     false, &err);
  /* Registration order defines firing priority: watchdog heartbeat first,
   * then the reply deadline, then the scheduler — matching the invariant
   * that a probe timeout is handled before the next probe is due. */
  if (wired) {
    wired =
        ls_loop_add_timer(&app->loop, on_watchdog_timer, app,
                          &app->watchdog_timer, &err) &&
        ls_loop_add_timer(&app->loop, on_reply_timer, app,
                          &app->reply_timer, &err) &&
        ls_loop_add_timer(&app->loop, on_ping_timer, app,
                          &app->ping_timer, &err);
  }
  if (!wired) {
    ls_error(&app->log, "Failed to set up event loop: %s", err.msg);
    ls_loop_destroy(&app->loop);
    return LS_EXIT_FAILURE;
  }

  uint64_t now_ms = ls_now_ms();
  if (now_ms == UINT64_MAX) {
    ls_error(&app->log, "Failed to initialize monotonic timing state");
    ls_loop_destroy(&app->loop);
    return LS_EXIT_FAILURE;
  }
  app->ping_timer->deadline_ms = now_ms; /* first probe fires immediately */
  if (app->watchdog_interval_ms > 0) {
    ls_timer_arm_after(app->watchdog_timer, now_ms, app->watchdog_interval_ms);
  }

  log_startup(app);
  int exit_code = ls_loop_run(&app->loop);
  log_shutdown(app, exit_code);
  ls_loop_destroy(&app->loop);
  return exit_code;
}
