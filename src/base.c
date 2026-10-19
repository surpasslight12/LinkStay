/*
 * base.c — dependency-free foundation: unified error type, monotonic clock,
 * duration formatting. No state, no configuration, no I/O beyond the clock.
 */

#include "base.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

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

void ls_format_latency(double ms, char *buf, size_t size) {
  if (ms < 0.0) {
    ms = 0.0;
  }
  if (ms < 1.0) {
    (void)snprintf(buf, size, "%.3fms", ms);
  } else if (ms < 100.0) {
    (void)snprintf(buf, size, "%.2fms", ms);
  } else {
    (void)snprintf(buf, size, "%.1fms", ms);
  }
}

void ls_format_uptime(uint64_t seconds, char *buf, size_t size) {
  uint64_t days = seconds / UINT64_C(86400);
  uint64_t hours = seconds / UINT64_C(3600) % 24;
  uint64_t minutes = seconds / UINT64_C(60) % 60;
  uint64_t secs = seconds % 60;

  if (days > 0) {
    (void)snprintf(buf, size,
                   "%" PRIu64 "d%" PRIu64 "h%" PRIu64 "m%" PRIu64 "s", days,
                   hours, minutes, secs);
  } else if (hours > 0) {
    (void)snprintf(buf, size, "%" PRIu64 "h%" PRIu64 "m%" PRIu64 "s", hours,
                   minutes, secs);
  } else if (minutes > 0) {
    (void)snprintf(buf, size, "%" PRIu64 "m%" PRIu64 "s", minutes, secs);
  } else {
    (void)snprintf(buf, size, "%" PRIu64 "s", secs);
  }
}
