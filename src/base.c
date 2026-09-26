/*
 * base.c — dependency-free foundation: unified error type, monotonic clock,
 * duration formatting. No state, no configuration, no I/O beyond the clock.
 */

#include "base.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

/* ---- Error type ---- */

bool ls_err_set(ls_err_t *err, const char *fmt, ...) {
  if (err != nullptr) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
    va_end(args);
  }
  return false;
}

/* ---- Monotonic clock ---- */

uint64_t ls_now_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0 &&
                  clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#else
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#endif
  /* CLOCK_MONOTONIC is non-negative on Linux; direct multiplication is
   * safe for any realistic uptime. */
  uint64_t seconds_ms = (uint64_t)ts.tv_sec * LS_MS_PER_SEC;
  return ls_add_sat(seconds_ms, (uint64_t)ts.tv_nsec / UINT64_C(1000000));
}

uint64_t ls_now_ns(void) {
  struct timespec ts;
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
  uint64_t seconds_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000);
  return ls_add_sat(seconds_ns, (uint64_t)ts.tv_nsec);
}

/* ---- Duration formatting ---- */

void ls_format_duration(uint64_t ms, char *buf, size_t size) {
  if (ms >= LS_MS_PER_SEC && ms % LS_MS_PER_SEC == 0) {
    (void)snprintf(buf, size, "%" PRIu64 "s", ms / LS_MS_PER_SEC);
  } else {
    (void)snprintf(buf, size, "%" PRIu64 "ms", ms);
  }
}
