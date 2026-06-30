#include "common.h"

uint64_t get_monotonic_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
  if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0 &&
                        clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#else
  if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#endif
  uint64_t seconds_ms = 0;
  if (LINKSTAY_UNLIKELY(
          ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, LINKSTAY_MS_PER_SEC))) {
    return UINT64_MAX;
  }
  uint64_t result = 0;
  if (LINKSTAY_UNLIKELY(
          ckd_add(&result, seconds_ms,
                  (uint64_t)ts.tv_nsec / UINT64_C(1000000)))) {
    return UINT64_MAX;
  }
  return result;
}
