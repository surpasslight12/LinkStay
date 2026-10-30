#ifndef LINKSTAY_BASE_H
#define LINKSTAY_BASE_H

/*
 * base.h — dependency-free foundation: program identity, exit codes, the
 * unified error type, monotonic clock, saturating math, and duration
 * formatting. Everything here is stateless; leveled logging lives in log.h and
 * IP literal parsing in addr.h.
 */

/* C23 is a hard requirement: the code relies on bool/nullptr/static_assert/
 * alignas being keywords, on [[maybe_unused]], and on <stdckdint.h>. GCC 14
 * reports 202000L for -std=c23; final C23 implementations report 202311L. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L
#error "linkstay requires C23; build with -std=c23 (see the Makefile)"
#endif

#include <stddef.h>
#include <stdint.h>

#define LS_VERSION "1.0"
#define LS_PROGRAM_NAME "linkstay"

/* Path to the shutdown command; validated during configuration loading
 * and used to trigger the real poweroff (app.c). */
#define LS_SYSTEMCTL_PATH "/usr/bin/systemctl"

/* Time unit conversion constants (integers); modules derive any float
 * conversions they need from these. */
#define LS_NS_PER_MS UINT64_C(1000000)  /* nanoseconds per millisecond */
#define LS_NS_PER_SEC UINT64_C(1000000000) /* nanoseconds per second */
#define LS_US_PER_MS UINT64_C(1000)     /* microseconds per millisecond */
#define LS_MS_PER_SEC UINT64_C(1000)    /* milliseconds per second */

/* Process exit codes: everything fatal maps to LS_EXIT_FAILURE. */
#define LS_EXIT_SUCCESS 0
#define LS_EXIT_FAILURE 1

#define LS_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Branch hints use the GCC/Clang __builtin_expect builtin. Vendor attributes
 * such as [[gnu::cold]] and [[gnu::format]] are standard C23 attribute syntax
 * with an implementation-defined namespace. */
#define LS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LS_LIKELY(x) __builtin_expect(!!(x), 1)

static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");

/* ---- Unified error type ---- */

typedef struct {
  char msg[256];
} ls_err_t;

[[gnu::format(printf, 2, 3)]] bool ls_err_set(ls_err_t *err, const char *fmt,
                                              ...);

/* ---- Monotonic clock ---- */

[[nodiscard]] uint64_t ls_now_ms(void);
[[nodiscard]] uint64_t ls_now_ns(void);

[[nodiscard]] static inline uint64_t ls_add_sat(uint64_t a, uint64_t b) {
  return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

/* Saturating multiplication; used where derived durations could overflow. */
[[nodiscard]] static inline uint64_t ls_mul_sat(uint64_t a, uint64_t b) {
  return b != 0 && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* ---- Duration formatting ---- */

/* Formats a millisecond duration compactly: whole seconds as "<n>s",
 * otherwise "<n>ms". Always NUL-terminates. */
void ls_format_duration(uint64_t ms, char *buf, size_t size);

/* Formats a latency in milliseconds, keeping sub-millisecond detail instead of
 * rounding fast links to "0.00ms": "0.032ms", "23.45ms", "150.3ms". Negative
 * input is clamped to zero. */
void ls_format_latency(double ms, char *buf, size_t size);

/* Formats an elapsed second count as the shortest exact d/h/m/s form:
 * "45s", "2m5s", "1h1m40s", "3d2h1m5s". Always NUL-terminates. */
void ls_format_uptime(uint64_t seconds, char *buf, size_t size);

#endif /* LINKSTAY_BASE_H */
