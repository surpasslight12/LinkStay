#ifndef LINKSTAY_COMMON_H
#define LINKSTAY_COMMON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * common.h — foundation layer.
 *
 * Project identity, generic macros, exit codes, and the monotonic clock.
 * Depends only on the C/POSIX headers so it can sit at the bottom of the
 * include hierarchy.
 */

#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LINKSTAY_VERSION "1.0"
#define LINKSTAY_PROGRAM_NAME "LinkStay"

#define LINKSTAY_MS_PER_SEC UINT64_C(1000)
#define LINKSTAY_EXIT_SUCCESS 0
#define LINKSTAY_EXIT_FAILURE 1

#define LINKSTAY_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#define LINKSTAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LINKSTAY_LIKELY(x) __builtin_expect(!!(x), 1)
#define LINKSTAY_COLD [[gnu::cold]]

static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");

/* Monotonic milliseconds since an unspecified epoch.
 * Returns UINT64_MAX on clock failure or arithmetic overflow. */
uint64_t get_monotonic_ms(void);

#endif /* LINKSTAY_COMMON_H */
