#ifndef LINKSTAY_CONFIG_H
#define LINKSTAY_CONFIG_H

/*
 * config.h — environment-resolved service configuration.
 *
 * The systemd unit (and optional /etc/linkstay.conf) supplies LINKSTAY_*
 * environment variables. Defaults are applied first, then every present
 * variable, then cross-field validation.
 */

#include "base.h"
#include "log.h"

typedef struct {
  /* Network */
  char target[64];      /* IPv4/IPv6 literal only; DNS intentionally rejected */
  uint64_t interval_ms; /* probe interval in milliseconds */
  int fail_threshold;   /* consecutive failures before shutdown */
  uint64_t timeout_ms;  /* per-probe reply timeout in milliseconds */
  uint64_t start_delay_ms; /* delay before the first probe */
  /* Shutdown */
  bool poweroff; /* false = simulate only (dry-run) */
  /* Logging */
  ls_log_level_t log_level;
} ls_config_t;

/* Resolves defaults and environment, then validates. On failure returns false
 * with err set. */
[[nodiscard]] bool ls_config_resolve(ls_config_t *restrict config,
                                     ls_err_t *restrict err);

/* Dumps the resolved configuration at debug level. */
void ls_config_dump(const ls_config_t *restrict config,
                    const ls_log_t *restrict log);

#endif /* LINKSTAY_CONFIG_H */
