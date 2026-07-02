#include "shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYSTEMCTL_PATH "/usr/bin/systemctl"
#define STARTUP_GRACE_MS 1000U
#define POLL_INTERVAL_NS 50000000L

/* Minimal environment for defense-in-depth and deterministic command output. */
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

/* Returns SHUTDOWN_RESULT_NO_ACTION when the child has not exited yet. */
static shutdown_result_t consume_child_status(pid_t child_pid,
                                              pid_t wait_result, int status,
                                              const logger_t *logger) {
  if (wait_result != child_pid) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    if (code == 0) {
      logger_info(logger, "Shutdown command accepted by systemd");
      return SHUTDOWN_RESULT_TRIGGERED;
    }
    logger_error(logger, "Shutdown command failed with exit code %d: %s",
                 code, SYSTEMCTL_PATH);
    return SHUTDOWN_RESULT_FAILED;
  }
  if (WIFSIGNALED(status)) {
    logger_error(logger, "Shutdown command terminated by signal %d",
                 WTERMSIG(status));
    return SHUTDOWN_RESULT_FAILED;
  }
  logger_error(logger, "Shutdown command exited unexpectedly");
  return SHUTDOWN_RESULT_FAILED;
}

/* Watch the spawned systemctl for up to STARTUP_GRACE_MS:
 *  - early non-zero exit ⇒ FAILED
 *  - clean exit         ⇒ TRIGGERED
 *  - still running      ⇒ assume hand-off succeeded, return TRIGGERED */
static shutdown_result_t observe_startup(pid_t child_pid,
                                         const logger_t *logger) {
  uint64_t start_ms = get_monotonic_ms();
  if (start_ms == UINT64_MAX) {
    logger_error(logger,
                 "Unable to confirm shutdown command result: monotonic clock "
                 "unavailable");
    return SHUTDOWN_RESULT_FAILED;
  }
  uint64_t deadline_ms = start_ms + STARTUP_GRACE_MS;

  while (true) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    shutdown_result_t r =
        consume_child_status(child_pid, result, status, logger);
    if (r != SHUTDOWN_RESULT_NO_ACTION) {
      return r;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      logger_error(logger, "waitpid() failed: %s", strerror(errno));
      return SHUTDOWN_RESULT_FAILED;
    }

    uint64_t now_ms = get_monotonic_ms();
    if (now_ms == UINT64_MAX) {
      logger_error(logger, "Unable to confirm shutdown command result: "
                           "monotonic clock unavailable");
      return SHUTDOWN_RESULT_FAILED;
    }
    if (now_ms >= deadline_ms) {
      logger_warn(logger, "Shutdown command did not exit within startup grace; "
                          "assuming request was handed off");
      return SHUTDOWN_RESULT_TRIGGERED;
    }
    if (!sleep_retry_window()) {
      logger_error(logger, "Startup observation sleep interrupted by "
                           "non-retryable error");
      return SHUTDOWN_RESULT_FAILED;
    }
  }
}

static shutdown_result_t spawn_shutdown_command(const logger_t *logger) {
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) {
    logger_error(logger, "posix_spawn_file_actions_init failed: %s",
                 strerror(rc));
    return SHUTDOWN_RESULT_FAILED;
  }
  if ((rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                             "/dev/null", O_RDONLY, 0)) == 0 &&
      (rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                             "/dev/null", O_WRONLY, 0)) == 0) {
    rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                          O_WRONLY, 0);
  }
  if (rc != 0) {
    logger_error(logger, "Failed to prepare stdio redirection: %s",
                 strerror(rc));
    posix_spawn_file_actions_destroy(&actions);
    return SHUTDOWN_RESULT_FAILED;
  }

  pid_t child_pid = -1;
  int spawn_err = posix_spawn(&child_pid, SHUTDOWN_ARGV[0], &actions, nullptr,
                              SHUTDOWN_ARGV, SHUTDOWN_ENVP);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_err != 0) {
    logger_error(logger, "posix_spawn failed: %s", strerror(spawn_err));
    return SHUTDOWN_RESULT_FAILED;
  }
  return observe_startup(child_pid, logger);
}

shutdown_result_t shutdown_trigger(const config_t *config,
                                   const logger_t *logger) {
  if (config == nullptr || logger == nullptr) {
    return SHUTDOWN_RESULT_FAILED;
  }

  logger_warn(logger, "Failure threshold reached, poweroff is %s",
              config->poweroff ? "true" : "false");

  if (!config->poweroff) {
    logger_info(logger,
                "[DRY-RUN] Would power off the system now (no action taken)");
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  logger_warn(logger, "Triggering system shutdown now");
  return spawn_shutdown_command(logger);
}
