#include "linkstay.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINKSTAY_SHUTDOWN_STARTUP_GRACE_MS 1000U
#define LINKSTAY_SHUTDOWN_POLL_INTERVAL_NS 50000000L

static const char *shutdown_command_path(void) { return "/usr/bin/systemctl"; }

static bool shutdown_select_argv(const config_t *restrict config, char *argv[],
                                 size_t argv_size) {
  if (config == NULL || argv == NULL || argv_size < 5) {
    return false;
  }

  for (size_t i = 0; i < argv_size; i++) {
    argv[i] = NULL;
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_TRUE_OFF) {
    argv[0] = (char *)shutdown_command_path();
    argv[1] = "--no-block";
    argv[2] = "poweroff";
    return true;
  }

  return false;
}

static bool shutdown_should_execute(const config_t *restrict config,
                                    const logger_t *restrict logger) {
  if (config == NULL || logger == NULL) {
    return false;
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_info(logger, "[DRY-RUN] Would trigger shutdown now");
    return false;
  }

  return true;
}

static bool shutdown_sleep_retry_window(void) {
  struct timespec remaining = {.tv_sec = 0,
                               .tv_nsec = LINKSTAY_SHUTDOWN_POLL_INTERVAL_NS};
  while (nanosleep(&remaining, &remaining) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

static shutdown_result_t
shutdown_observation_failed(const logger_t *restrict logger,
                            const char *restrict reason) {
  if (logger != NULL && reason != NULL) {
    logger_error(logger, "Unable to confirm shutdown command result: %s",
                 reason);
  }

  return SHUTDOWN_RESULT_FAILED;
}

static shutdown_result_t
shutdown_consume_child_status(pid_t child_pid, pid_t wait_result, int status,
                              const char *restrict command_path,
                              const logger_t *restrict logger) {
  if (wait_result != child_pid) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  if (WIFEXITED(status)) {
    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
      logger_info(logger, "Shutdown command executed successfully");
      return SHUTDOWN_RESULT_TRIGGERED;
    }

    logger_error(logger, "Shutdown command failed with exit code %d: %s",
                 exit_code, command_path);
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

static shutdown_result_t
shutdown_observe_startup(pid_t child_pid, const char *restrict command_path,
                         const logger_t *restrict logger) {
  if (child_pid <= 0 || command_path == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  uint64_t start_ms = get_monotonic_ms();
  if (start_ms == UINT64_MAX) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result < 0) {
      if (errno == EINTR) {
        return shutdown_observation_failed(
            logger, "monotonic clock unavailable during startup observation");
      }

      logger_error(logger, "waitpid() failed: %s", strerror(errno));
      return SHUTDOWN_RESULT_FAILED;
    }

    shutdown_result_t immediate_result = shutdown_consume_child_status(
        child_pid, result, status, command_path, logger);
    if (immediate_result != SHUTDOWN_RESULT_NO_ACTION) {
      return immediate_result;
    }

    return shutdown_observation_failed(
        logger, "monotonic clock unavailable during startup observation");
  }

  uint64_t deadline_ms = 0;
  if (ckd_add(&deadline_ms, start_ms,
              (uint64_t)LINKSTAY_SHUTDOWN_STARTUP_GRACE_MS)) {
    return shutdown_observation_failed(logger,
                                       "startup observation deadline overflow");
  }

  int status = 0;
  while (true) {
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    shutdown_result_t child_result = shutdown_consume_child_status(
        child_pid, result, status, command_path, logger);
    if (child_result != SHUTDOWN_RESULT_NO_ACTION) {
      return child_result;
    }

    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }

      logger_error(logger, "waitpid() failed: %s", strerror(errno));
      return SHUTDOWN_RESULT_FAILED;
    }

    uint64_t now_ms = get_monotonic_ms();
    if (now_ms == UINT64_MAX || now_ms >= deadline_ms) {
      if (now_ms == UINT64_MAX) {
        return shutdown_observation_failed(
            logger, "monotonic clock unavailable before startup grace elapsed");
      }

      logger_warn(logger, "Shutdown command did not exit within startup grace; "
                          "assuming request was handed off");
      return SHUTDOWN_RESULT_TRIGGERED;
    }

    if (!shutdown_sleep_retry_window()) {
      return shutdown_observation_failed(
          logger,
          "startup observation sleep interrupted by non-retryable error");
    }
  }
}

static shutdown_result_t
shutdown_execute_command(char *argv[], const logger_t *restrict logger) {
  if (argv == NULL || argv[0] == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  /* Minimal environment for defense-in-depth and deterministic command output.
   */
  static char *const shutdown_envp[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin",
                                        "LANG=C", "LC_ALL=C", NULL};

  posix_spawn_file_actions_t file_actions;
  int rc = posix_spawn_file_actions_init(&file_actions);
  if (rc != 0) {
    logger_error(logger, "posix_spawn_file_actions_init failed: %s",
                 strerror(rc));
    return SHUTDOWN_RESULT_FAILED;
  }

  rc = posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO,
                                        "/dev/null", O_RDONLY, 0);
  if (rc == 0) {
    rc = posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO,
                                          "/dev/null", O_WRONLY, 0);
  }
  if (rc == 0) {
    rc = posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO,
                                          "/dev/null", O_WRONLY, 0);
  }
  if (rc != 0) {
    logger_error(logger, "Failed to prepare stdio redirection: %s",
                 strerror(rc));
    posix_spawn_file_actions_destroy(&file_actions);
    return SHUTDOWN_RESULT_FAILED;
  }

  pid_t child_pid = -1;
  int spawn_err = posix_spawn(&child_pid, argv[0], &file_actions, NULL, argv,
                              shutdown_envp);

  posix_spawn_file_actions_destroy(&file_actions);

  if (spawn_err != 0) {
    logger_error(logger, "posix_spawn failed: %s", strerror(spawn_err));
    return SHUTDOWN_RESULT_FAILED;
  }

  return shutdown_observe_startup(child_pid, argv[0], logger);
}

shutdown_result_t shutdown_trigger(const config_t *config,
                                   const logger_t *logger) {
  if (config == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    logger_warn(logger, "Failure threshold reached in LOG-ONLY mode; "
                        "continuing monitoring without shutdown");
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  logger_warn(logger, "Shutdown threshold reached, mode is %s",
              shutdown_mode_to_string(config->shutdown_mode));

  if (!shutdown_should_execute(config, logger)) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  char *argv[5] = {0};
  if (!shutdown_select_argv(config, argv, LINKSTAY_ARRAY_LEN(argv))) {
    logger_error(logger, "Failed to build shutdown command arguments");
    return SHUTDOWN_RESULT_FAILED;
  }

  logger_warn(logger, "Triggering shutdown now");
  return shutdown_execute_command(argv, logger);
}
