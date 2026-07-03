#ifndef LINKSTAY_NOTIFY_H
#define LINKSTAY_NOTIFY_H

/*
 * notify.h — sd_notify integration over the notify socket.
 *
 * Implements READY/STATUS/WATCHDOG/STOPPING notifications directly, without
 * linking libsystemd. Status messages are de-duplicated within a short
 * window to keep socket traffic low. All operations are no-ops when the
 * notifier is disabled, so callers never need to guard their calls.
 */

#include "base.h"

#define LS_NOTIFY_STATUS_SIZE 240U
#define LS_NOTIFY_DEDUP_WINDOW_MS UINT64_C(2000)

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LS_NOTIFY_STATUS_SIZE];
} ls_notify_t;

/* Enables the notifier when NOTIFY_SOCKET is present and connectable;
 * otherwise leaves it disabled. Never fails hard. */
void ls_notify_init(ls_notify_t *restrict notify);
void ls_notify_destroy(ls_notify_t *restrict notify);
[[nodiscard]] bool ls_notify_enabled(const ls_notify_t *restrict notify);

bool ls_notify_ready(ls_notify_t *restrict notify);
bool ls_notify_status(ls_notify_t *restrict notify,
                      const char *restrict status);
[[gnu::format(printf, 2, 3)]] bool
ls_notify_statusf(ls_notify_t *restrict notify, const char *restrict fmt, ...);
bool ls_notify_stopping(ls_notify_t *restrict notify);
bool ls_notify_watchdog(ls_notify_t *restrict notify);

/* Watchdog heartbeat interval (half of WATCHDOG_USEC); 0 = no watchdog. */
[[nodiscard]] uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify);

#endif /* LINKSTAY_NOTIFY_H */
