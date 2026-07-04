#ifndef LINKSTAY_BASE_H
#define LINKSTAY_BASE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * base.h — dependency-free foundation.
 *
 * Project identity, generic macros, the unified error type, and the
 * monotonic clock. Every other module includes this header.
 */

#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LS_VERSION "1.0"
#define LS_PROGRAM_NAME "linkstay"

#define LS_MS_PER_SEC UINT64_C(1000)
#define LS_EXIT_SUCCESS 0
#define LS_EXIT_FAILURE 1

#define LS_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#define LS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LS_LIKELY(x) __builtin_expect(!!(x), 1)
#define LS_COLD [[gnu::cold]]

static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");

/*
 * Unified error convention: every fallible init/parse/resolve function
 * returns bool and, on failure, writes a human-readable message into a
 * caller-owned ls_err_t. This is the single error-reporting style across
 * the project — no ad-hoc (char *buf, size_t size) pairs.
 */
typedef struct {
  char msg[256];
} ls_err_t;

/* Formats err->msg and returns false, so call sites can write
 * `return ls_err_set(err, "...", ...);` in one line. NULL err is tolerated. */
[[gnu::format(printf, 2, 3)]] bool ls_err_set(ls_err_t *err, const char *fmt,
                                              ...);

/* Monotonic milliseconds since an unspecified epoch.
 * Returns UINT64_MAX on clock failure or arithmetic overflow. */
[[nodiscard]] uint64_t ls_now_ms(void);

/* Monotonic nanoseconds since an unspecified epoch, for latency measurement
 * (uses the fine-grained clock, unlike the coarse scheduling clock above).
 * Returns UINT64_MAX on clock failure or arithmetic overflow. */
[[nodiscard]] uint64_t ls_now_ns(void);

/* a + b with saturation at UINT64_MAX instead of wrap-around. */
[[nodiscard]] static inline uint64_t ls_add_sat(uint64_t a, uint64_t b) {
  uint64_t result = 0;
  return ckd_add(&result, a, b) ? UINT64_MAX : result;
}

#endif /* LINKSTAY_BASE_H */
