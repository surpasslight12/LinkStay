#ifndef LINKSTAY_ACTION_H
#define LINKSTAY_ACTION_H

/*
 * action.h — threshold action backend (system shutdown).
 *
 * When poweroff is enabled, invokes `systemctl --no-block poweroff` via
 * posix_spawn and observes the startup window; when disabled, only logs a
 * dry-run message. No other action backends exist by design.
 */

#include "base.h"
#include "log.h"

typedef enum {
  LS_ACTION_SIMULATED = 0, /* dry-run: logged, nothing executed */
  LS_ACTION_TRIGGERED = 1, /* shutdown handed off to systemd */
  LS_ACTION_FAILED = 2,    /* command could not be started or failed early */
} ls_action_result_t;

ls_action_result_t ls_action_shutdown(bool poweroff,
                                      const ls_log_t *restrict log);

#endif /* LINKSTAY_ACTION_H */
