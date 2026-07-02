#ifndef LINKSTAY_CONFIG_H
#define LINKSTAY_CONFIG_H

/*
 * config.h — runtime configuration.
 *
 * config_resolve() is the single orchestration path that layers defaults,
 * environment variables, and CLI arguments (in that precedence order) and
 * validates the result. Targets are IPv4/IPv6 literals only; DNS is
 * intentionally rejected.
 */

#include "common.h"
#include "logger.h"

/* Target buffer: IPv6 max literal is 45 chars; 64 is ample. */
typedef struct {
  /* Network */
  char target[64];
  int interval_sec;
  int fail_threshold;
  int timeout_ms;

  /* Shutdown: true powers the system off via systemctl; false only simulates. */
  bool poweroff;

  /* Logging */
  log_level_t log_level;

  /* Integration */
  bool enable_systemd;
} config_t;

[[nodiscard]] bool config_resolve(config_t *restrict config, int argc,
                                  char **restrict argv,
                                  bool *restrict exit_requested,
                                  char *restrict error_msg, size_t error_size);
[[nodiscard]] bool config_log_timestamps_enabled(const config_t *restrict config);
void config_print(const config_t *restrict config,
                  const logger_t *restrict logger);

#endif /* LINKSTAY_CONFIG_H */
