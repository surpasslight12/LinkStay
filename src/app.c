/*
 * app.c — process entry and application assembly: probe scheduling, threshold
 * action, startup/shutdown banners, and main().
 *
 * Sections are ordered so every helper is defined before its first use: no
 * forward declarations are needed. Everything here is private except main().
 * Mechanisms live in their own modules (addr/log/notify/icmp/loop/stats);
 * this file owns policy and orchestration.
 */

#include "addr.h"
#include "base.h"
#include "icmp.h"
#include "log.h"
#include "loop.h"
#include "notify.h"
#include "opts.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LS_PACKET_SIZE 64U
#define LS_MAX_REPLY_DRAIN_PER_WAKEUP 32U

/* Compile-time guardrails: the assembly layer relies on these capacities. */
static_assert(LS_PACKET_SIZE <= LS_ICMP_SEND_BUFFER_SIZE,
              "echo packet must fit in the ICMP send buffer");
static_assert(LS_LOOP_MAX_FDS >= 2,
              "app needs the signalfd and the ICMP socket");
static_assert(LS_LOOP_MAX_TIMERS >= 3,
              "app needs ping, reply, and watchdog timer slots");

/* Double conversion factor for latency measurement; derived from the integer
 * unit constants in base.h so they cannot drift apart. */
#define LS_NS_PER_MS_F ((double)LS_NS_PER_MS)

/* The only runtime safety warning worth keeping: real poweroff is dangerous
 * with an extremely low threshold. */
#define LS_WARN_THRESHOLD_MIN_WITH_POWEROFF 3

/* Implementation-private constants. */
#define LS_STARTUP_GRACE_MS UINT64_C(1000)
#define LS_SHUTDOWN_RETRY_MS 50U

typedef enum {
  LS_ACTION_SIMULATED = 0,
  LS_ACTION_TRIGGERED = 1,
  LS_ACTION_FAILED = 2,
} action_result_t;

typedef struct {
  /* Resolved configuration (copied in by app_init). */
  ls_opts_t opts;
  ls_log_t log;

  /* Runtime state. */
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

  uint64_t probe_sent_ns;
  uint16_t probe_sequence;
  int consecutive_fails;

  bool watchdog_last_failed;
  bool signal_stop;
} app_t;

/* ---- Shutdown action (systemctl poweroff) ---- */

static char *const SHUTDOWN_ENVP[] = {
    "PATH=/usr/bin:/usr/sbin:/bin:/sbin", "LANG=C", "LC_ALL=C", nullptr};

static char *const SHUTDOWN_ARGV[] = {
    (char *)LS_SYSTEMCTL_PATH, "--no-block", "poweroff", nullptr};

static bool sleep_retry_window(void) {
  struct timespec remaining = {
      .tv_sec = 0,
      .tv_nsec = (long)LS_SHUTDOWN_RETRY_MS * (long)LS_NS_PER_MS,
  };
  while (nanosleep(&remaining, &remaining) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

static action_result_t consume_child_status(int status, const ls_log_t *log) {
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

/* systemctl --no-block should exit quickly; if it does not, assume the request
 * was handed off rather than blocking the monitor indefinitely. */
static action_result_t observe_startup(pid_t child_pid, const ls_log_t *log) {
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

static action_result_t spawn_shutdown_command(const ls_log_t *log) {
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) {
    ls_error(log, "posix_spawn_file_actions_init() failed: %s", strerror(rc));
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
  int spawn_err = posix_spawn(&child_pid, SHUTDOWN_ARGV[0], &actions, nullptr,
                              SHUTDOWN_ARGV, SHUTDOWN_ENVP);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_err != 0) {
    ls_error(log, "posix_spawn() failed: %s", strerror(spawn_err));
    return LS_ACTION_FAILED;
  }
  return observe_startup(child_pid, log);
}

static action_result_t run_shutdown_action(bool poweroff,
                                           const ls_log_t *log) {
  if (!poweroff) {
    ls_warn(log, "Failure threshold reached; poweroff is disabled (dry-run)");
    ls_info(log, "[DRY-RUN] Would run %s --no-block poweroff",
            LS_SYSTEMCTL_PATH);
    return LS_ACTION_SIMULATED;
  }
  ls_warn(log, "Failure threshold reached; triggering system shutdown now");
  return spawn_shutdown_command(log);
}

/* ---- Error path ---- */

/* Logs, mirrors the message to the systemd status, and stops the loop. */
static void app_fail(app_t *app, const char *what, const ls_err_t *err) {
  ls_error(&app->log, "%s: %s", what, err->msg);
  (void)ls_notify_statusf(&app->notify, "Error: %s: %s", what, err->msg);
  ls_loop_stop(&app->loop, LS_EXIT_FAILURE);
}

/* ---- Statistics reporting ---- */

static void log_statistics(app_t *app) {
  const ls_stats_t *s = &app->stats;
  char uptime_str[32];
  ls_format_uptime(ls_stats_uptime_sec(s), uptime_str, sizeof(uptime_str));
  uint64_t total = ls_stats_total(s);
  const char *plural = total == 1 ? "" : "s";

  if (s->ok > 0) {
    char min_str[32];
    char avg_str[32];
    char max_str[32];
    ls_format_latency(ls_stats_min_latency_ms(s), min_str, sizeof(min_str));
    ls_format_latency(ls_stats_avg_latency_ms(s), avg_str, sizeof(avg_str));
    ls_format_latency(ls_stats_max_latency_ms(s), max_str, sizeof(max_str));
    ls_info(&app->log,
            "Statistics: uptime %s | %" PRIu64 " ping%s, %" PRIu64
            " OK, %" PRIu64 " failed (%.2f%% success) | latency min %s, "
            "avg %s, max %s",
            uptime_str, total, plural, s->ok, s->failed,
            ls_stats_success_rate(s), min_str, avg_str, max_str);
  } else {
    ls_info(&app->log,
            "Statistics: uptime %s | %" PRIu64 " ping%s, 0 OK, %" PRIu64
            " failed (0.00%% success) | latency N/A",
            uptime_str, total, plural, s->failed);
  }
}

/* ---- Probe lifecycle ---- */

static void probe_reset(app_t *app) {
  app->probe_sent_ns = 0;
  app->probe_sequence = 0;
  ls_timer_disarm(app->reply_timer);
}

/* Records one failed probe (reply timeout or rejected send) and refreshes the
 * systemd status; callers run check_threshold() afterwards. */
static void probe_failed(app_t *app, const char *reason) {
  app->consecutive_fails++;
  ls_stats_add_fail(&app->stats);
  ls_warn(&app->log, "Probe to %s failed: %s (failure %d of %d)",
          app->opts.target, reason, app->consecutive_fails,
          app->opts.fail_threshold);
  (void)ls_notify_statusf(&app->notify,
                          "Unreachable: %s, %d/%d consecutive failures",
                          app->opts.target, app->consecutive_fails,
                          app->opts.fail_threshold);
}

static void probe_succeeded(app_t *app, uint16_t sequence,
                            double latency_ms) {
  app->consecutive_fails = 0;
  ls_stats_add_ok(&app->stats, latency_ms);

  char latency_str[32];
  char avg_str[32];
  char uptime_str[32];
  ls_format_latency(latency_ms, latency_str, sizeof(latency_str));
  ls_format_latency(ls_stats_avg_latency_ms(&app->stats), avg_str,
                    sizeof(avg_str));
  ls_format_uptime(ls_stats_uptime_sec(&app->stats), uptime_str,
                   sizeof(uptime_str));

  uint64_t total = ls_stats_total(&app->stats);
  ls_debug(&app->log,
           "Reply from %s: seq=%u time=%s avg=%s ok=%" PRIu64 "/%" PRIu64
           " (%.1f%%)",
           app->opts.target, (unsigned)sequence, latency_str, avg_str,
           app->stats.ok, total, ls_stats_success_rate(&app->stats));
  (void)ls_notify_statusf(&app->notify,
                          "Online: %s up %s, last %s, avg %s, %" PRIu64
                          "/%" PRIu64 " OK (%.1f%%)",
                          app->opts.target, uptime_str, latency_str, avg_str,
                          app->stats.ok, total,
                          ls_stats_success_rate(&app->stats));
}

/* Runs the threshold action once enough consecutive failures accumulate. */
static void check_threshold(app_t *app) {
  if (app->consecutive_fails < app->opts.fail_threshold) {
    return;
  }

  action_result_t result = run_shutdown_action(app->opts.poweroff, &app->log);
  if (result == LS_ACTION_SIMULATED) {
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

/* Drains the ICMP socket non-blocking and processes the first matching reply
 * for the outstanding probe. Returns true when a matching reply was accepted
 * (probe state already reset and statistics recorded), false when the socket
 * is exhausted without a match.
 *
 * On receive errors this calls app_fail (which stops the loop), so the caller
 * must check ls_loop_stopping() after a false return. */
static bool drain_and_match_reply(app_t *app) {
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
        ls_timer_armed(app->reply_timer)) {
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
      probe_succeeded(app, sequence, latency_ms);
      return true;
    }
  }
  return false;
}

/* ---- Loop callbacks ---- */

static void on_ping_timer(ls_loop_t *loop, void *userdata) {
  app_t *app = userdata;
  uint64_t now_ms = ls_loop_now(loop);

  /* Always keep the periodic schedule, but never overlap probes. With
   * timeout < interval (validated in opts.c), an outstanding probe here is
   * unusual. */
  ls_timer_step(app->ping_timer, app->opts.interval_ms, now_ms);
  if (ls_timer_armed(app->reply_timer)) {
    return;
  }

  ls_err_t err = {};
  ls_icmp_send_status_t status =
      ls_icmp_send_echo(&app->icmp, &app->dest, app->dest_len, app->identifier,
                        LS_PACKET_SIZE, &err);
  if (status == LS_ICMP_SEND_ERROR) {
    app_fail(app, "Failed to send ICMP echo", &err);
    return;
  }
  if (status == LS_ICMP_SEND_UNREACHABLE) {
    /* The kernel refused delivery (no route, link down, buffer pressure).
     * That is exactly the outage the threshold logic exists for, so count it
     * as a failed probe instead of aborting the monitor. */
    probe_failed(app, err.msg);
    check_threshold(app);
    return;
  }

  app->probe_sent_ns = ls_now_ns();
  app->probe_sequence = app->icmp.sequence;
  ls_timer_arm_after(app->reply_timer, now_ms, app->opts.timeout_ms);
}

static void on_reply_timer([[maybe_unused]] ls_loop_t *loop, void *userdata) {
  app_t *app = userdata;

  /* Last-chance non-blocking drain: the reply may have landed in the
   * socket buffer between the most recent poll wakeup and this timer
   * firing. If we can consume it now we avoid a spurious failure. */
  if (drain_and_match_reply(app)) {
    return; /* Reply arrived just in time — success already recorded */
  }
  if (ls_loop_stopping(&app->loop)) {
    return; /* drain_and_match_reply triggered a fatal error */
  }

  /* Genuine timeout — no matching reply was waiting. */
  probe_reset(app);
  char timeout_str[32];
  char reason[64];
  ls_format_duration(app->opts.timeout_ms, timeout_str, sizeof(timeout_str));
  (void)snprintf(reason, sizeof(reason), "no reply within %s", timeout_str);
  probe_failed(app, reason);
  check_threshold(app);
}

static void on_watchdog_timer(ls_loop_t *loop, void *userdata) {
  app_t *app = userdata;
  bool sent = ls_notify_watchdog(&app->notify);
  ls_timer_step(app->watchdog_timer,
                ls_notify_watchdog_interval_ms(&app->notify),
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

static void on_icmp_readable([[maybe_unused]] ls_loop_t *loop,
                             [[maybe_unused]] int fd, void *userdata) {
  app_t *app = userdata;
  (void)drain_and_match_reply(app);
}

static void on_signal(ls_loop_t *loop, uint32_t signo, void *userdata) {
  app_t *app = userdata;
  if (signo == SIGINT || signo == SIGTERM) {
    app->signal_stop = true;
    ls_loop_stop(loop, LS_EXIT_SUCCESS);
  } else if (signo == SIGUSR1) {
    log_statistics(app);
  }
}

/* ---- Startup / shutdown banners ---- */

static void log_startup(app_t *app) {
  uint64_t detection_ms = ls_mul_sat((uint64_t)app->opts.fail_threshold,
                                     app->opts.interval_ms);
  char interval_str[32];
  char timeout_str[32];
  char detection_str[32];
  char delay_str[32];
  char first_probe_str[64];
  ls_format_duration(app->opts.interval_ms, interval_str, sizeof(interval_str));
  ls_format_duration(app->opts.timeout_ms, timeout_str, sizeof(timeout_str));
  ls_format_duration(detection_ms, detection_str, sizeof(detection_str));
  ls_format_duration(app->opts.start_delay_ms, delay_str, sizeof(delay_str));
  if (app->opts.start_delay_ms == 0) {
    (void)snprintf(first_probe_str, sizeof(first_probe_str), "immediate");
  } else {
    (void)snprintf(first_probe_str, sizeof(first_probe_str), "in %s",
                   delay_str);
  }
  ls_info(&app->log,
          LS_PROGRAM_NAME " %s monitoring %s | first probe %s | interval %s, "
          "timeout %s, threshold %d (detection ~%s), poweroff %s",
          LS_VERSION, app->opts.target, first_probe_str, interval_str,
          timeout_str, app->opts.fail_threshold, detection_str,
          app->opts.poweroff ? "true" : "false (dry-run)");
  if (!ls_notify_ready(&app->notify)) {
    ls_warn(&app->log, "Failed to send systemd READY notification");
  }
  if (app->opts.start_delay_ms == 0) {
    (void)ls_notify_statusf(&app->notify,
                            "Monitoring %s every %s; poweroff %s",
                            app->opts.target, interval_str,
                            app->opts.poweroff ? "true" : "false (dry-run)");
  } else {
    (void)ls_notify_statusf(&app->notify,
                            "Waiting %s before first probe; poweroff %s",
                            delay_str,
                            app->opts.poweroff ? "true" : "false (dry-run)");
  }
}

static void log_shutdown(app_t *app, int exit_code) {
  if (app->signal_stop) {
    ls_info(&app->log, "Shutdown signal received, stopping gracefully");
  }
  /* Notify systemd we are stopping on every exit path (signal, threshold
   * dry-run, or real poweroff) so the service manager always sees a clean
   * transition. */
  (void)ls_notify_stopping(&app->notify);
  log_statistics(app);
  if (exit_code == LS_EXIT_SUCCESS) {
    ls_info(&app->log, LS_PROGRAM_NAME " monitor stopped");
  }
}

/* ---- Application lifecycle ---- */

static bool app_init(app_t *app, const ls_opts_t *opts, ls_err_t *err) {
  if (app == nullptr || opts == nullptr) {
    return ls_err_set(err, "Invalid application init arguments");
  }

  *app = (app_t){
      .opts = *opts,
      .identifier = (uint16_t)(getpid() & 0xFFFF),
      /* Keep destroy safe if init bails before the sockets are opened. */
      .icmp = {.sockfd = -1},
      .notify = {.sockfd = -1},
  };
  if (app->identifier == 0) {
    app->identifier = 1;
  }

  ls_log_init(&app->log, app->opts.log_level);
  if (app->opts.log_level == LS_LOG_DEBUG) {
    ls_opts_dump(&app->opts, &app->log);
  }

  app->dest_len = sizeof(app->dest);
  if (!ls_addr_parse(app->opts.target, &app->dest, &app->dest_len, err)) {
    return false;
  }

  /* systemd is the only supported runtime: establish NOTIFY_SOCKET before
   * opening the ICMP socket so a manual invocation fails immediately. */
  if (!ls_notify_init(&app->notify, err)) {
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
  } else {
    ls_debug(&app->log, "ICMP BPF filter active for %s traffic",
             family == AF_INET6 ? "IPv6" : "IPv4");
  }

  ls_stats_init(&app->stats);

  uint64_t watchdog_interval_ms = ls_notify_watchdog_interval_ms(&app->notify);
  if (watchdog_interval_ms > 0) {
    ls_debug(&app->log, "systemd watchdog ping every %" PRIu64 "ms",
             watchdog_interval_ms);
  } else {
    ls_debug(&app->log, "systemd watchdog disabled");
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

static void app_destroy(app_t *app) {
  if (app == nullptr) {
    return;
  }
  ls_notify_destroy(&app->notify);
  ls_icmp_close(&app->icmp);
  *app = (app_t){};
}

static int app_run(app_t *app) {
  if (app == nullptr) {
    return LS_EXIT_FAILURE;
  }

  ls_err_t err = {};
  if (!ls_loop_init(&app->loop, &app->log, &err)) {
    ls_error(&app->log, "%s", err.msg);
    return LS_EXIT_FAILURE;
  }

  static const int WATCHED_SIGNALS[] = {SIGINT, SIGTERM, SIGUSR1};
  bool wired =
      ls_loop_watch_signals(&app->loop, WATCHED_SIGNALS,
                            LS_ARRAY_LEN(WATCHED_SIGNALS), on_signal, app,
                            &err) &&
      ls_loop_add_fd(&app->loop, app->icmp.sockfd, on_icmp_readable, app,
                     false, &err);
  /* Registration order defines firing priority: watchdog heartbeat first,
   * then the reply deadline, then the scheduler — matching the invariant
   * that a probe timeout is handled before the next probe is due. */
  if (wired) {
    wired =
        ls_loop_add_timer(&app->loop, on_watchdog_timer, app,
                          &app->watchdog_timer, &err) &&
        ls_loop_add_timer(&app->loop, on_reply_timer, app, &app->reply_timer,
                          &err) &&
        ls_loop_add_timer(&app->loop, on_ping_timer, app, &app->ping_timer,
                          &err);
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
  ls_timer_arm_after(app->ping_timer, now_ms, app->opts.start_delay_ms);
  uint64_t watchdog_interval_ms = ls_notify_watchdog_interval_ms(&app->notify);
  if (watchdog_interval_ms > 0) {
    ls_timer_arm_after(app->watchdog_timer, now_ms, watchdog_interval_ms);
  }

  log_startup(app);
  int exit_code = ls_loop_run(&app->loop);
  log_shutdown(app, exit_code);
  ls_loop_destroy(&app->loop);
  return exit_code;
}

/* ---- Process entry point ---- */

/* Logs a fatal startup error. Before the environment resolves only the error
 * level is known; afterwards the resolved level applies. */
static void log_fatal_error(ls_log_level_t level, const char *message) {
  ls_log_t log;
  ls_log_init(&log, level);
  ls_error(&log, LS_PROGRAM_NAME " failed: %s", message);
}

int main(void) {
  ls_err_t err = {};
  ls_opts_t opts;

  if (!ls_opts_resolve(&opts, &err)) {
    log_fatal_error(LS_LOG_ERROR, err.msg);
    return LS_EXIT_FAILURE;
  }

  /* Zero-initialized so app_destroy() is safe even if app_init() fails
   * before it finishes assigning *app. */
  app_t app = {};
  if (!app_init(&app, &opts, &err)) {
    log_fatal_error(opts.log_level, err.msg);
    app_destroy(&app);
    return LS_EXIT_FAILURE;
  }

  int rc = app_run(&app);
  if (rc != LS_EXIT_SUCCESS) {
    ls_error(&app.log, LS_PROGRAM_NAME " exited with code %d", rc);
  }
  app_destroy(&app);
  return rc;
}
