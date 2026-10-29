#ifndef LINKSTAY_NOTIFY_H
#define LINKSTAY_NOTIFY_H

/*
 * notify.h — sd_notify(3) channel for the systemd service manager.
 *
 * Owns the AF_UNIX datagram channel and the READY/STATUS/WATCHDOG/STOPPING
 * protocol, including the NOTIFY_SOCKET, WATCHDOG_USEC and WATCHDOG_PID
 * environment contract. The channel is required: linkstay is a systemd service,
 * so ls_notify_init() fails when NOTIFY_SOCKET is absent.
 */

#include "base.h"

#define LS_NOTIFY_STATUS_SIZE 240U

typedef struct {
  bool opened; /* ownership flag; false for a zero-initialized struct */
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LS_NOTIFY_STATUS_SIZE];
} ls_notify_t;

/* Connects to NOTIFY_SOCKET and reads WATCHDOG_USEC/WATCHDOG_PID. Returns false
 * with err set when the process is not running under systemd or the channel is
 * unusable. The struct is always left destroy-safe. */
[[nodiscard]] bool ls_notify_init(ls_notify_t *restrict notify,
                                  ls_err_t *restrict err);

/* Closes the channel. Safe on a zero-initialized struct and after a failed
 * ls_notify_init(). */
void ls_notify_destroy(ls_notify_t *restrict notify);

[[nodiscard]] bool ls_notify_ready(ls_notify_t *restrict notify);
[[nodiscard]] [[gnu::format(printf, 2, 3)]] bool ls_notify_statusf(
    ls_notify_t *restrict notify, const char *restrict fmt, ...);
[[nodiscard]] bool ls_notify_stopping(ls_notify_t *restrict notify);
[[nodiscard]] bool ls_notify_watchdog(ls_notify_t *restrict notify);

/* Watchdog ping interval in milliseconds: half of WATCHDOG_USEC (systemd
 * convention), or 0 when the watchdog is not requested. */
[[nodiscard]] uint64_t
ls_notify_watchdog_interval_ms(const ls_notify_t *restrict notify);

#endif /* LINKSTAY_NOTIFY_H */
