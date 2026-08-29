#include "app.h"

/* ---- Fatal-error logging ----
 *
 * Logs a fatal startup error. Before options resolve only the error level
 * is known; afterwards the resolved level and timestamp policy apply. */

static void log_fatal_error(ls_log_level_t level, bool timestamps,
                            const char *restrict message) {
  ls_log_t log;
  ls_log_init(&log, level, timestamps);
  ls_error(&log, LS_PROGRAM_NAME " failed: %s", message);
}

/* ---- Entry point ---- */

int main(int argc, char **argv) {
  ls_err_t err = {};
  bool exit_requested = false;
  ls_opts_t opts;

  if (!ls_opts_resolve(&opts, argc, argv, &exit_requested, &err)) {
    log_fatal_error(LS_LOG_ERROR, false, err.msg);
    return LS_EXIT_FAILURE;
  }
  if (exit_requested) {
    return LS_EXIT_SUCCESS;
  }

  ls_app_t app;
  if (!ls_app_init(&app, &opts, &err)) {
    log_fatal_error(opts.log_level, ls_opts_timestamps(&opts), err.msg);
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
