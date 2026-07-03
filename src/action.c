#include "action.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYSTEMCTL_PATH "/usr/bin/systemctl"
#define STARTUP_GRACE_MS 1000U
#define POLL_INTERVAL_NS 50000000L

/* Minimal environment for defense-in-depth and deterministic output. */
static char *const SHUTDOWN_ENVP[] = {
    "PATH=/usr/bin:/usr/sbin:/bin:/sbin", "LANG=C", "LC_ALL=C", nullptr};

static char *const SHUTDOWN_ARGV[] = {(char *)SYSTEMCTL_PATH, "--no-block",
                                      "poweroff", nullptr};

static bool sleep_retry_window(void) {
  struct timespec remaining = {.tv_sec = 0, .tv_nsec = POLL_INTERVAL_NS};
  while (nanosleep(&remaining, &remaining) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

/* Interprets one waitpid() observation; SIMULATED = child still running. */
static ls_action_result_t consume_child_status(pid_t child_pid,
                                               pid_t wait_result, int status,
                                               const ls_log_t *log) {
  if (wait_result != child_pid) {
    return LS_ACTION_SIMULATED; /* not exited yet */
  }
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    if (code == 0) {
      ls_info(log, "Shutdown command accepted by systemd");
      return LS_ACTION_TRIGGERED;
    }
    ls_error(log, "Shutdown command failed with exit code %d: %s", code,
             SYSTEMCTL_PATH);
    return LS_ACTION_FAILED;
  }
  if (WIFSIGNALED(status)) {
    ls_error(log, "Shutdown command terminated by signal %d", WTERMSIG(status));
    return LS_ACTION_FAILED;
  }
  ls_error(log, "Shutdown command exited unexpectedly");
  return LS_ACTION_FAILED;
}

/* Watches the spawned systemctl for up to STARTUP_GRACE_MS:
 *  - early non-zero exit ⇒ FAILED
 *  - clean exit          ⇒ TRIGGERED
 *  - still running       ⇒ assume hand-off succeeded, TRIGGERED */
static ls_action_result_t observe_startup(pid_t child_pid,
                                          const ls_log_t *log) {
  uint64_t start_ms = ls_now_ms();
  if (start_ms == UINT64_MAX) {
    ls_error(log, "Unable to confirm shutdown command result: monotonic "
                  "clock unavailable");
    return LS_ACTION_FAILED;
  }
  uint64_t deadline_ms = ls_add_sat(start_ms, STARTUP_GRACE_MS);

  while (true) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      return consume_child_status(child_pid, result, status, log);
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
  int spawn_err = posix_spawn(&child_pid, SHUTDOWN_ARGV[0], &actions, nullptr,
                              SHUTDOWN_ARGV, SHUTDOWN_ENVP);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_err != 0) {
    ls_error(log, "posix_spawn failed: %s", strerror(spawn_err));
    return LS_ACTION_FAILED;
  }
  return observe_startup(child_pid, log);
}

ls_action_result_t ls_action_shutdown(bool poweroff,
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
