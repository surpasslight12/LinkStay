#include "base.h"

#include <stdarg.h>
#include <stdio.h>

bool ls_err_set(ls_err_t *err, const char *fmt, ...) {
  if (err != nullptr) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
    va_end(args);
  }
  return false;
}

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
  uint64_t seconds_ms = 0;
  if (LS_UNLIKELY(ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, LS_MS_PER_SEC))) {
    return UINT64_MAX;
  }
  return ls_add_sat(seconds_ms, (uint64_t)ts.tv_nsec / UINT64_C(1000000));
}

uint64_t ls_now_ns(void) {
  struct timespec ts;
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
  uint64_t seconds_ns = 0;
  if (LS_UNLIKELY(ckd_mul(&seconds_ns, (uint64_t)ts.tv_sec,
                          UINT64_C(1000000000)))) {
    return UINT64_MAX;
  }
  return ls_add_sat(seconds_ns, (uint64_t)ts.tv_nsec);
}
