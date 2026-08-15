#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define LS_PACKET_SIZE 64U
#define LS_MAX_REPLY_DRAIN_PER_WAKEUP 32U

/* Nanoseconds-to-milliseconds conversion (kept as double for latency). */
#define LS_NS_PER_MS_F 1000000.0

/* The only runtime safety warning worth keeping: real poweroff is dangerous
 * with an extremely low threshold. */
#define LS_WARN_THRESHOLD_MIN_WITH_POWEROFF 3

/* Implementation-private constants for the statistics/action/notify helpers
 * defined later in this file. */
#define LS_STATS_NO_SAMPLE UINT64_MAX
#define LS_US_PER_MS_F 1000.0
#define LS_SYSTEMCTL_PATH "/usr/bin/systemctl"
#define LS_STARTUP_GRACE_MS 1000U
#define LS_POLL_INTERVAL_NS 50000000L
#define LS_NOTIFY_MESSAGE_SIZE 256U
#define LS_NOTIFY_DEDUP_WINDOW_MS UINT64_C(2000)

typedef enum {
  LS_ACTION_SIMULATED = 0,
  LS_ACTION_TRIGGERED = 1,
  LS_ACTION_FAILED = 2,
} ls_action_result_t;

/* Forward declarations for the private implementation sections below. */
static void ls_stats_init(ls_stats_t *stats);
static void ls_stats_add_ok(ls_stats_t *stats, double latency_ms);
static void ls_stats_add_fail(ls_stats_t *stats);
[[nodiscard]] static double ls_stats_success_rate(const ls_stats_t *stats);
[[nodiscard]] static double ls_stats_avg_latency(const ls_stats_t *stats);
[[nodiscard]] static double ls_stats_latency_min_ms(const ls_stats_t *stats);
[[nodiscard]] static double ls_stats_latency_max_ms(const ls_stats_t *stats);
[[nodiscard]] static uint64_t ls_stats_uptime_sec(const ls_stats_t *stats);
static ls_action_result_t ls_action_shutdown(bool poweroff,
                                             const ls_log_t *restrict log);
static void ls_notify_init(ls_notify_t *restrict notify);
static void ls_notify_destroy(ls_notify_t *restrict notify);
[[nodiscard]] static bool ls_notify_enabled(const ls_notify_t *restrict notify);
static bool ls_notify_ready(ls_notify_t *restrict notify);
[[gnu::format(printf, 2, 3)]] static bool
ls_notify_statusf(ls_notify_t *restrict notify, const char *restrict fmt, ...);
static bool ls_notify_stopping(ls_notify_t *restrict notify);
static bool ls_notify_watchdog(ls_notify_t *restrict notify);
[[nodiscard]] static uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify);

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
  uint64_t detection_sec =
      (uint64_t)app->opts.fail_threshold * (uint64_t)app->opts.interval_sec;
  ls_info(&app->log,
          LS_PROGRAM_NAME " %s monitoring %s | interval %ds, timeout %dms, " \
          "threshold %d (detection ~%" PRIu64 "s), poweroff %s",
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

/* ===================================================================
 *  Statistics
 * =================================================================== */

static void ls_stats_init(ls_stats_t *stats) {
  if (stats == nullptr) {
    return;
  }
  *stats = (ls_stats_t){
      .latency_min_us = LS_STATS_NO_SAMPLE,
      .latency_max_us = LS_STATS_NO_SAMPLE,
      .started_at_ms = ls_now_ms(),
  };
}

static void ls_stats_add_ok(ls_stats_t *stats, double latency_ms) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  if (LS_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  if (LS_LIKELY(stats->total < UINT64_MAX)) {
    stats->total++;
  }
  if (LS_LIKELY(stats->ok < UINT64_MAX)) {
    stats->ok++;
  }
  uint64_t latency_us = (uint64_t)(latency_ms * LS_US_PER_MS_F);
  stats->latency_sum_us = ls_add_sat(stats->latency_sum_us, latency_us);
  if (latency_us < stats->latency_min_us) {
    stats->latency_min_us = latency_us;
  }
  if (latency_us > stats->latency_max_us) {
    stats->latency_max_us = latency_us;
  }
}

static void ls_stats_add_fail(ls_stats_t *stats) {
  if (LS_UNLIKELY(stats == nullptr)) {
    return;
  }
  if (LS_LIKELY(stats->total < UINT64_MAX)) {
    stats->total++;
  }
  if (LS_LIKELY(stats->failed < UINT64_MAX)) {
    stats->failed++;
  }
}

static double ls_stats_success_rate(const ls_stats_t *stats) {
  if (stats == nullptr || stats->total == 0) {
    return 0.0;
  }
  return (double)stats->ok / (double)stats->total * 100.0;
}

static double ls_stats_avg_latency(const ls_stats_t *stats) {
  if (stats == nullptr || stats->ok == 0) {
    return 0.0;
  }
  return (double)stats->latency_sum_us / (double)stats->ok / LS_US_PER_MS_F;
}

static double ls_stats_latency_min_ms(const ls_stats_t *stats) {
  if (stats == nullptr || stats->latency_min_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_min_us / LS_US_PER_MS_F;
}

static double ls_stats_latency_max_ms(const ls_stats_t *stats) {
  if (stats == nullptr || stats->latency_max_us == LS_STATS_NO_SAMPLE) {
    return 0.0;
  }
  return (double)stats->latency_max_us / LS_US_PER_MS_F;
}

static uint64_t ls_stats_uptime_sec(const ls_stats_t *stats) {
  if (stats == nullptr) {
    return 0;
  }
  uint64_t now_ms = ls_now_ms();
  if (now_ms == UINT64_MAX || stats->started_at_ms == UINT64_MAX ||
      now_ms < stats->started_at_ms) {
    return 0;
  }
  return (now_ms - stats->started_at_ms) / LS_MS_PER_SEC;
}

/* ===================================================================
 *  Threshold action (systemctl poweroff)
 * =================================================================== */

static char *const LS_SHUTDOWN_ENVP[] = {
    "PATH=/usr/bin:/usr/sbin:/bin:/sbin", "LANG=C", "LC_ALL=C", nullptr};

static char *const LS_SHUTDOWN_ARGV[] = {
    (char *)LS_SYSTEMCTL_PATH, "--no-block", "poweroff", nullptr};

static bool sleep_retry_window(void) {
  struct timespec remaining = {.tv_sec = 0, .tv_nsec = LS_POLL_INTERVAL_NS};
  while (nanosleep(&remaining, &remaining) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

static ls_action_result_t consume_child_status(int status,
                                               const ls_log_t *log) {
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    if (code == 0) {
      ls_info(log, "Shutdown command accepted by systemd");
      return LS_ACTION_TRIGGERED;
    }
    ls_error(log, "Shutdown command failed with exit code %d: %s", code,
             LS_SYSTEMCTL_PATH);
    return LS_ACTION_FAILED;
  }
  if (WIFSIGNALED(status)) {
    ls_error(log, "Shutdown command terminated by signal %d", WTERMSIG(status));
    return LS_ACTION_FAILED;
  }
  ls_error(log, "Shutdown command exited unexpectedly");
  return LS_ACTION_FAILED;
}

static ls_action_result_t observe_startup(pid_t child_pid,
                                          const ls_log_t *log) {
  uint64_t start_ms = ls_now_ms();
  if (start_ms == UINT64_MAX) {
    ls_error(log, "Unable to confirm shutdown command result: monotonic "
                  "clock unavailable");
    return LS_ACTION_FAILED;
  }
  uint64_t deadline_ms = ls_add_sat(start_ms, LS_STARTUP_GRACE_MS);

  while (true) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      return consume_child_status(status, log);
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      ls_error(log, "waitpid() failed: %s", strerror(errno));
      return LS_ACTION_FAILED;
    }

    uint64_t now_ms = ls_now_ms();
    if (now_ms == UINT64_MAX) {
      ls_error(log, "Unable to confirm shutdown command result: monotonic "
                    "clock unavailable");
      return LS_ACTION_FAILED;
    }
    if (now_ms >= deadline_ms) {
      ls_warn(log, "Shutdown command did not exit within startup grace; "
                   "assuming request was handed off");
      return LS_ACTION_TRIGGERED;
    }
    if (!sleep_retry_window()) {
      ls_error(log,
               "Startup observation sleep interrupted by non-retryable error");
      return LS_ACTION_FAILED;
    }
  }
}

static ls_action_result_t spawn_shutdown_command(const ls_log_t *log) {
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) {
    ls_error(log, "posix_spawn_file_actions_init failed: %s", strerror(rc));
    return LS_ACTION_FAILED;
  }
  if ((rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                             "/dev/null", O_RDONLY, 0)) == 0 &&
      (rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                             "/dev/null", O_WRONLY, 0)) == 0) {
    rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                          O_WRONLY, 0);
  }
  if (rc != 0) {
    ls_error(log, "Failed to prepare stdio redirection: %s", strerror(rc));
    posix_spawn_file_actions_destroy(&actions);
    return LS_ACTION_FAILED;
  }

  pid_t child_pid = -1;
  int spawn_err = posix_spawn(&child_pid, LS_SHUTDOWN_ARGV[0], &actions,
                              nullptr, LS_SHUTDOWN_ARGV, LS_SHUTDOWN_ENVP);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_err != 0) {
    ls_error(log, "posix_spawn failed: %s", strerror(spawn_err));
    return LS_ACTION_FAILED;
  }
  return observe_startup(child_pid, log);
}

static ls_action_result_t ls_action_shutdown(bool poweroff,
                                             const ls_log_t *restrict log) {
  ls_warn(log, "Failure threshold reached, poweroff is %s",
          poweroff ? "true" : "false");

  if (!poweroff) {
    ls_info(log, "[DRY-RUN] Would power off the system now (no action taken)");
    return LS_ACTION_SIMULATED;
  }

  ls_warn(log, "Triggering system shutdown now");
  return spawn_shutdown_command(log);
}

/* ===================================================================
 *  systemd notify integration
 * =================================================================== */

static bool parse_uint64(const char *restrict value,
                         uint64_t *restrict out_value) {
  if (value == nullptr) {
    return false;
  }
  errno = 0;
  char *endptr = nullptr;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0') {
    return false;
  }
  uint64_t converted = (uint64_t)parsed;
  if (converted != parsed) {
    return false;
  }
  *out_value = converted;
  return true;
}

static bool watchdog_pid_matches_self(void) {
  const char *pid_str = getenv("WATCHDOG_PID");
  if (pid_str == nullptr || pid_str[0] == '\0') {
    return true;
  }
  errno = 0;
  char *endptr = nullptr;
  long parsed = strtol(pid_str, &endptr, 10);
  if (errno != 0 || endptr == pid_str || *endptr != '\0' || parsed <= 0) {
    return false;
  }
  return (pid_t)parsed == getpid();
}

static bool build_socket_addr(const char *restrict path,
                              struct sockaddr_un *restrict addr,
                              socklen_t *restrict addr_len) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  if (path[0] == '@') {
    size_t name_len = strlen(path + 1);
    if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
      return false;
    }
    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, path + 1, name_len);
    *addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return true;
  }

  size_t path_len = strlen(path);
  if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
    return false;
  }
  memcpy(addr->sun_path, path, path_len + 1);
  *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len +
                          1);
  return true;
}

static bool send_message(ls_notify_t *restrict notify,
                         const char *restrict message) {
  if (LS_UNLIKELY(notify == nullptr || message == nullptr ||
                  !notify->enabled)) {
    return false;
  }

  ssize_t sent;
  do {
    sent = send(notify->sockfd, message, strlen(message), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent >= 0;
}

static bool notify_status(ls_notify_t *restrict notify,
                          const char *restrict status) {
  if (notify == nullptr || !notify->enabled || status == nullptr) {
    return false;
  }

  uint64_t now_ms = ls_now_ms();
  bool same = strcmp(notify->last_status, status) == 0;
  if (same && notify->last_status_ms != 0 && now_ms != UINT64_MAX &&
      now_ms - notify->last_status_ms < LS_NOTIFY_DEDUP_WINDOW_MS) {
    return true;
  }

  char message[LS_NOTIFY_MESSAGE_SIZE];
  (void)snprintf(message, sizeof(message), "STATUS=%.*s",
                 (int)LS_NOTIFY_STATUS_SIZE - 1, status);
  bool ok = send_message(notify, message);
  if (ok) {
    (void)snprintf(notify->last_status, sizeof(notify->last_status), "%s",
                   status);
    notify->last_status_ms = (now_ms == UINT64_MAX) ? 0 : now_ms;
  }
  return ok;
}

static void ls_notify_init(ls_notify_t *restrict notify) {
  if (notify == nullptr) {
    return;
  }
  *notify = (ls_notify_t){.sockfd = -1};

  const char *socket_path = getenv("NOTIFY_SOCKET");
  if (socket_path == nullptr) {
    return;
  }

  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    return;
  }

  struct sockaddr_un addr;
  socklen_t addr_len;
  if (!build_socket_addr(socket_path, &addr, &addr_len)) {
    close(fd);
    return;
  }

  if (connect(fd, (const struct sockaddr *)&addr, addr_len) != 0) {
    close(fd);
    return;
  }

  notify->sockfd = fd;
  notify->enabled = true;

  const char *watchdog_str = getenv("WATCHDOG_USEC");
  if (watchdog_str != nullptr && watchdog_pid_matches_self()) {
    uint64_t usec = 0;
    if (parse_uint64(watchdog_str, &usec)) {
      notify->watchdog_usec = usec;
    }
  }
}

static void ls_notify_destroy(ls_notify_t *restrict notify) {
  if (notify == nullptr) {
    return;
  }
  if (notify->sockfd >= 0) {
    close(notify->sockfd);
    notify->sockfd = -1;
  }
  notify->enabled = false;
}

static bool ls_notify_enabled(const ls_notify_t *restrict notify) {
  return notify != nullptr && notify->enabled;
}

static bool ls_notify_ready(ls_notify_t *restrict notify) {
  return send_message(notify, "READY=1");
}

static bool ls_notify_statusf(ls_notify_t *restrict notify,
                            const char *restrict fmt, ...) {
  if (notify == nullptr || !notify->enabled || fmt == nullptr) {
    return false;
  }
  char status[LS_NOTIFY_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(status, sizeof(status), fmt, args);
  va_end(args);
  return notify_status(notify, status);
}

static bool ls_notify_stopping(ls_notify_t *restrict notify) {
  return send_message(notify, "STOPPING=1");
}

static bool ls_notify_watchdog(ls_notify_t *restrict notify) {
  if (LS_UNLIKELY(notify == nullptr || !notify->enabled ||
                  notify->watchdog_usec == 0)) {
    return false;
  }
  return send_message(notify, "WATCHDOG=1");
}

static uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify) {
  if (notify == nullptr || !notify->enabled || notify->watchdog_usec == 0) {
    return 0;
  }
  uint64_t interval_ms = notify->watchdog_usec / (LS_MS_PER_SEC * 2);
  return interval_ms == 0 ? 1 : interval_ms;
}

/* ---- Application assembly ---- */
