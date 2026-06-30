#ifndef LINKSTAY_MONITOR_H
#define LINKSTAY_MONITOR_H

/*
 * monitor.h — reactor and orchestration.
 *
 * linkstay_ctx_t aggregates configuration, the resolved destination address,
 * logger, metrics, ICMP state, and the systemd notifier. The reactor loop in
 * monitor.c is timer/state-machine driven on top of poll() + signalfd; it
 * owns no threads.
 */

#include "config.h"
#include "icmp.h"
#include "logger.h"
#include "metrics.h"
#include "systemd.h"

#include <signal.h>

typedef struct linkstay_context {
  volatile sig_atomic_t stop_flag;
  int consecutive_fails;
  uint16_t cached_pid; /* cached getpid() & 0xFFFF, avoids syscall in hot path */

  config_t config;
  struct sockaddr_storage dest_addr;
  socklen_t dest_addr_len;
  logger_t logger;
  metrics_t metrics;
  icmp_pinger_t pinger;
  systemd_notifier_t systemd;
} linkstay_ctx_t;

static_assert(sizeof(sig_atomic_t) >= sizeof(int),
              "sig_atomic_t must hold an int");

bool linkstay_ctx_init(linkstay_ctx_t *restrict ctx,
                       const config_t *restrict config,
                       char *restrict error_msg, size_t error_size);
void linkstay_ctx_destroy(linkstay_ctx_t *restrict ctx);
int linkstay_reactor_run(linkstay_ctx_t *restrict ctx);

#endif /* LINKSTAY_MONITOR_H */