#ifndef LINKSTAY_SYSTEMD_H
#define LINKSTAY_SYSTEMD_H

/*
 * systemd.h — sd_notify integration over the notify socket.
 *
 * Implements READY/STATUS/WATCHDOG/STOPPING notifications directly, without
 * linking libsystemd. Status messages are de-duplicated within a short window
 * to keep socket traffic low.
 */

#include "common.h"

#define LINKSTAY_SYSTEMD_MESSAGE_SIZE 256U
#define LINKSTAY_SYSTEMD_STATUS_SIZE 240U
#define LINKSTAY_STATUS_DEDUP_WINDOW_MS UINT64_C(2000)

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LINKSTAY_SYSTEMD_STATUS_SIZE];
} systemd_notifier_t;

void systemd_notifier_init(systemd_notifier_t *restrict notifier);
void systemd_notifier_destroy(systemd_notifier_t *restrict notifier);
[[nodiscard]] bool systemd_notifier_is_enabled(
    const systemd_notifier_t *restrict notifier);
bool systemd_notifier_ready(systemd_notifier_t *restrict notifier);
bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                             const char *restrict status);
bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier);
bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier);
[[nodiscard]] uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t *restrict notifier);

#endif /* LINKSTAY_SYSTEMD_H */
