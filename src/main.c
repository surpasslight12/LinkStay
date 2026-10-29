/*
 * main.c — systemd process entry point: resolve the service environment, run
 * the application, and map the result to an exit code. It owns no monitoring
 * logic and accepts no command-line arguments.
 */

#include "app.h"

#include "log.h"

/* ---- Fatal-error logging ---- */
/* Logs a fatal startup error. Before the environment resolves only the error
 * level is known; afterwards the resolved level applies. */

static void log_fatal_error(ls_log_level_t level, const char *message) {
  ls_log_t log;
  ls_log_init(&log, level);
  ls_error(&log, LS_PROGRAM_NAME " failed: %s", message);
}

/* ---- Entry point ---- */

int main(void) {
  ls_err_t err = {};
  ls_opts_t opts;

  if (!ls_opts_resolve(&opts, &err)) {
    log_fatal_error(LS_LOG_ERROR, err.msg);
    return LS_EXIT_FAILURE;
  }

  /* Zero-initialized so ls_app_destroy() is safe even if ls_app_init() fails
   * before it finishes assigning *app. */
  ls_app_t app = {};
  if (!ls_app_init(&app, &opts, &err)) {
    log_fatal_error(opts.log_level, err.msg);
    ls_app_destroy(&app);
    return LS_EXIT_FAILURE;
  }

  int rc = ls_app_run(&app);
  if (rc != LS_EXIT_SUCCESS) {
    ls_error(&app.log, LS_PROGRAM_NAME " exited with code %d", rc);
  }
  ls_app_destroy(&app);
  return rc;
}
