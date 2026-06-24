#ifndef LINKSTAY_RUNTIME_H
#define LINKSTAY_RUNTIME_H

/*
 * runtime.h — runtime services abstraction.
 *
 * Decouples the monitor loop from any specific integration backend. Today the
 * only backend is systemd; new integrations should fit this vtable instead of
 * adding ad hoc branches throughout the monitor loop. The function pointers
 * use type-safe wrapper functions (not casts) to avoid UB per C23 6.5.2.2.
 */

#include "common.h"
#include "systemd.h"

typedef struct {
  void *backend_ctx;
  bool enabled;
  bool (*ready)(void *backend_ctx);
  bool (*status)(void *backend_ctx, const char *status);
  bool (*stopping)(void *backend_ctx);
  bool (*watchdog)(void *backend_ctx);
  uint64_t (*watchdog_interval_ms)(const void *backend_ctx);
  void (*destroy)(void *backend_ctx);
} runtime_services_t;

void runtime_services_init(runtime_services_t *restrict services,
                           systemd_notifier_t *restrict systemd,
                           bool enable_systemd);
void runtime_services_destroy(runtime_services_t *restrict services);
bool runtime_services_is_enabled(const runtime_services_t *restrict services);
uint64_t runtime_services_watchdog_interval_ms(
    const runtime_services_t *restrict services);
bool runtime_services_notify_status(runtime_services_t *restrict services,
                                    const char *restrict status);
bool runtime_services_notify_ready(runtime_services_t *restrict services);
bool runtime_services_notify_watchdog(runtime_services_t *restrict services);
bool runtime_services_notify_stopping(runtime_services_t *restrict services);

#endif /* LINKSTAY_RUNTIME_H */
