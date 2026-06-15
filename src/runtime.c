#include "runtime.h"

/* ---- No-op backend (null object) ---- */

static bool runtime_services_noop_ready(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static bool runtime_services_noop_status(void *backend_ctx,
                                         const char *status) {
  (void)backend_ctx;
  (void)status;
  return true;
}

static bool runtime_services_noop_stopping(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static bool runtime_services_noop_watchdog(void *backend_ctx) {
  (void)backend_ctx;
  return true;
}

static uint64_t
runtime_services_noop_watchdog_interval_ms(const void *backend_ctx) {
  (void)backend_ctx;
  return 0;
}

static void runtime_services_noop_destroy(void *backend_ctx) {
  (void)backend_ctx;
}

static void runtime_services_set_null(runtime_services_t *restrict services) {
  if (services == NULL) {
    return;
  }

  services->backend_ctx = NULL;
  services->enabled = false;
  services->ready = runtime_services_noop_ready;
  services->status = runtime_services_noop_status;
  services->stopping = runtime_services_noop_stopping;
  services->watchdog = runtime_services_noop_watchdog;
  services->watchdog_interval_ms = runtime_services_noop_watchdog_interval_ms;
  services->destroy = runtime_services_noop_destroy;
}

/* ---- systemd backend ----
 * Type-safe wrappers: avoid UB from casting incompatible function pointers. */

static bool runtime_services_systemd_ready(void *ctx) {
  return systemd_notifier_ready((systemd_notifier_t *)ctx);
}

static bool runtime_services_systemd_status(void *ctx, const char *status) {
  return systemd_notifier_status((systemd_notifier_t *)ctx, status);
}

static bool runtime_services_systemd_stopping(void *ctx) {
  return systemd_notifier_stopping((systemd_notifier_t *)ctx);
}

static bool runtime_services_systemd_watchdog(void *ctx) {
  return systemd_notifier_watchdog((systemd_notifier_t *)ctx);
}

static uint64_t runtime_services_systemd_watchdog_interval_ms(const void *ctx) {
  return systemd_notifier_watchdog_interval_ms((const systemd_notifier_t *)ctx);
}

static void runtime_services_systemd_destroy(void *ctx) {
  systemd_notifier_destroy((systemd_notifier_t *)ctx);
}

/* ---- Public API ---- */

void runtime_services_init(runtime_services_t *restrict services,
                           systemd_notifier_t *restrict systemd,
                           bool enable_systemd) {
  if (services == NULL) {
    return;
  }

  runtime_services_set_null(services);
  if (!enable_systemd || systemd == NULL) {
    return;
  }

  systemd_notifier_init(systemd);
  if (!systemd_notifier_is_enabled(systemd)) {
    return;
  }

  services->backend_ctx = systemd;
  services->enabled = true;
  services->ready = runtime_services_systemd_ready;
  services->status = runtime_services_systemd_status;
  services->stopping = runtime_services_systemd_stopping;
  services->watchdog = runtime_services_systemd_watchdog;
  services->watchdog_interval_ms = runtime_services_systemd_watchdog_interval_ms;
  services->destroy = runtime_services_systemd_destroy;
}

void runtime_services_destroy(runtime_services_t *restrict services) {
  if (services == NULL) {
    return;
  }

  services->destroy(services->backend_ctx);
  runtime_services_set_null(services);
}

bool runtime_services_is_enabled(const runtime_services_t *restrict services) {
  return services != NULL && services->enabled;
}

uint64_t runtime_services_watchdog_interval_ms(
    const runtime_services_t *restrict services) {
  if (services == NULL) {
    return 0;
  }

  return services->watchdog_interval_ms(services->backend_ctx);
}

bool runtime_services_notify_status(runtime_services_t *restrict services,
                                    const char *restrict status) {
  if (services == NULL || status == NULL) {
    return false;
  }

  if (!runtime_services_is_enabled(services)) {
    return true;
  }

  return services->status(services->backend_ctx, status);
}

bool runtime_services_notify_ready(runtime_services_t *restrict services) {
  return services != NULL && services->ready(services->backend_ctx);
}

bool runtime_services_notify_watchdog(runtime_services_t *restrict services) {
  return services != NULL && services->watchdog(services->backend_ctx);
}

bool runtime_services_notify_stopping(runtime_services_t *restrict services) {
  return services != NULL && services->stopping(services->backend_ctx);
}
