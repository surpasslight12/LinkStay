#ifndef LINKSTAY_SHUTDOWN_H
#define LINKSTAY_SHUTDOWN_H

/*
 * shutdown.h — shutdown backend.
 *
 * Invokes `systemctl --no-block poweroff` for true-off while preserving
 * dry-run and log-only semantics. Uses posix_spawn() plus startup
 * observation rather than shelling out.
 */

#include "config.h"
#include "logger.h"

typedef enum {
  SHUTDOWN_RESULT_NO_ACTION = 0,
  SHUTDOWN_RESULT_TRIGGERED = 1,
  SHUTDOWN_RESULT_FAILED = 2,
} shutdown_result_t;

shutdown_result_t shutdown_trigger(const config_t *config,
                                   const logger_t *logger);

#endif /* LINKSTAY_SHUTDOWN_H */
