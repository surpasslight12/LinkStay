#ifndef LINKSTAY_OPTS_H
#define LINKSTAY_OPTS_H

/*
 * opts.h — environment-resolved service configuration.
 *
 * linkstay is a systemd service, not a CLI: the unit (and optional
 * /etc/linkstay.conf) supplies LINKSTAY_* environment variables. Defaults are
 * applied first, then every present LINKSTAY_* variable, then cross-field
 * validation.
 */

#include "base.h"
#include "log.h"

typedef struct {
  /* Network */
  char target[64];      /* IPv4/IPv6 literal only; DNS intentionally rejected */
  uint64_t interval_ms; /* probe interval in milliseconds */
  int fail_threshold;   /* consecutive failures before shutdown */
  uint64_t timeout_ms;  /* per-probe reply timeout in milliseconds */
  /* Shutdown */
  bool poweroff; /* false = simulate only (dry-run) */
  /* Logging */
  ls_log_level_t log_level;
} ls_opts_t;

/* Resolves defaults and environment, then validates. On failure returns false
 * with err set. */
[[nodiscard]] bool ls_opts_resolve(ls_opts_t *restrict opts,
                                   ls_err_t *restrict err);

/* Dumps the resolved configuration at debug level. */
void ls_opts_dump(const ls_opts_t *restrict opts, const ls_log_t *restrict log);

#endif /* LINKSTAY_OPTS_H */
