#ifndef LINKSTAY_OPTS_H
#define LINKSTAY_OPTS_H

/*
 * opts.h — resolved runtime options.
 *
 * opts.c resolves options from three layers, lowest priority first:
 * built-in defaults, LINKSTAY_* environment variables, then CLI arguments.
 * A cli_seen[] bitmap lets valid CLI options override even invalid env
 * values. Cross-field validation runs last.
 */

#include "base.h"

typedef struct {
  /* Network */
  char target[64]; /* IPv4/IPv6 literal only; DNS intentionally rejected */
  int interval_sec;
  int fail_threshold;
  int timeout_ms;
  /* Shutdown */
  bool poweroff; /* false = simulate only (dry-run) */
  /* Logging */
  ls_log_level_t log_level;
  /* Integration */
  bool systemd;
} ls_opts_t;

/* Resolves defaults → CLI → environment, then validates. On success returns
 * true; *exit_requested is set when --help/--version was handled (caller
 * should exit 0 without starting the monitor). */
[[nodiscard]] bool ls_opts_resolve(ls_opts_t *restrict opts, int argc,
                                   char **restrict argv,
                                   bool *restrict exit_requested,
                                   ls_err_t *restrict err);

/* Log timestamps are tied to systemd integration: journald already
 * timestamps every line, so they are enabled only when systemd is off. */
[[nodiscard]] bool ls_opts_timestamps(const ls_opts_t *restrict opts);

/* Dumps the resolved configuration at debug level. */
void ls_opts_dump(const ls_opts_t *restrict opts, const ls_log_t *restrict log);

#endif /* LINKSTAY_OPTS_H */
