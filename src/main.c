#include "app.h"

/* ---- Early-error logging ---- */

static void log_early_error(bool timestamps, const char *restrict message) {
  ls_log_t log;
  ls_log_init(&log, LS_LOG_ERROR, timestamps);
  ls_error(&log, LS_PROGRAM_NAME " failed: %s", message);
}

static void log_resolved_error(const ls_opts_t *restrict opts,
                               const char *restrict message) {
  ls_log_t log;
  ls_log_init(&log, opts->log_level, ls_opts_timestamps(opts));
  ls_error(&log, LS_PROGRAM_NAME " failed: %s", message);
}

/* ---- Entry point ---- */

int main(int argc, char **argv) {
  ls_err_t err = {};
  bool exit_requested = false;
  ls_opts_t opts;

  if (!ls_opts_resolve(&opts, argc, argv, &exit_requested, &err)) {
    log_early_error(false, err.msg);
    return LS_EXIT_FAILURE;
  }
  if (exit_requested) {
    return LS_EXIT_SUCCESS;
  }

  ls_app_t app;
  if (!ls_app_init(&app, &opts, &err)) {
    log_resolved_error(&opts, err.msg);
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
