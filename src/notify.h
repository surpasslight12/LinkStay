#ifndef LINKSTAY_NOTIFY_H
#define LINKSTAY_NOTIFY_H

/*
 * notify.h — sd_notify(3)-style integration backend.
 *
 * Owns the AF_UNIX datagram channel to the service manager and the
 * READY/STATUS/WATCHDOG/STOPPING protocol, including the NOTIFY_SOCKET,
 * WATCHDOG_USEC and WATCHDOG_PID environment contract. Every operation is a
 * no-op unless the channel was established, so callers never branch on
 * availability. This is an integration backend, not policy: when to send which
 * message stays in app.c.
 */

#include "base.h"

#define LS_NOTIFY_STATUS_SIZE 240U

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LS_NOTIFY_STATUS_SIZE];
} ls_notify_t;

/* Outcome of starting the notifier. */
typedef enum {
  LS_NOTIFY_ACTIVE = 0, /* channel established; watchdog may still be off */
  LS_NOTIFY_INACTIVE,   /* NOTIFY_SOCKET unset: systemd is not supervising */
  LS_NOTIFY_FAILED,     /* NOTIFY_SOCKET set but unusable; err explains */
} ls_notify_state_t;

/* Connects to NOTIFY_SOCKET and reads WATCHDOG_USEC/WATCHDOG_PID. Only
 * LS_NOTIFY_FAILED is an error condition, and even then the monitor keeps
 * running — callers warn and continue. On any outcome the struct is left in a
 * destroy-safe state. */
[[nodiscard]] ls_notify_state_t
ls_notify_init(ls_notify_t *restrict notify, ls_err_t *restrict err);

/* Closes the channel. Safe to call on a zero-initialized struct, and after a
 * failed ls_notify_init(). */
void ls_notify_destroy(ls_notify_t *restrict notify);

[[nodiscard]] bool ls_notify_enabled(const ls_notify_t *restrict notify);

[[nodiscard]] bool ls_notify_ready(ls_notify_t *restrict notify);
[[gnu::format(printf, 2, 3)]] bool ls_notify_statusf(
    ls_notify_t *restrict notify, const char *restrict fmt, ...);
[[nodiscard]] bool ls_notify_stopping(ls_notify_t *restrict notify);
[[nodiscard]] bool ls_notify_watchdog(ls_notify_t *restrict notify);

/* Watchdog ping interval in milliseconds: half of WATCHDOG_USEC (systemd
 * convention), or 0 when the watchdog is not requested. */
[[nodiscard]] uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify);

#endif /* LINKSTAY_NOTIFY_H */
