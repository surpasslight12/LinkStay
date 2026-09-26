#ifndef LINKSTAY_BASE_H
#define LINKSTAY_BASE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* C23 is a hard requirement: the code relies on bool/nullptr/static_assert/
 * alignas being keywords, on [[maybe_unused]], and on <stdckdint.h>. GCC 14
 * reports 202000L for -std=c23 and GCC 15 reports 202311L, so the test is
 * "newer than C17" rather than an exact revision. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ <= 201710L
#error "linkstay requires C23; build with -std=c23 (see the Makefile)"
#endif

/*
 * base.h — dependency-free foundation: program identity, exit codes, the
 * unified error type, monotonic clock, saturating math, and duration
 * formatting. Everything here is stateless; leveled logging lives in log.h and
 * IP literal parsing in addr.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LS_VERSION "1.0"
#define LS_PROGRAM_NAME "linkstay"

/* Path to the shutdown command; validated at option resolution (opts.c) and
 * used to trigger the real poweroff (app.c). */
#define LS_SYSTEMCTL_PATH "/usr/bin/systemctl"

/* Time unit conversion constants (integers; float conversions for latency
 * math live in app.c and are derived from these). */
#define LS_NS_PER_MS UINT64_C(1000000) /* nanoseconds per millisecond */
#define LS_US_PER_MS UINT64_C(1000)    /* microseconds per millisecond */
#define LS_MS_PER_SEC UINT64_C(1000)   /* milliseconds per second */
#define LS_EXIT_SUCCESS 0
#define LS_EXIT_FAILURE 1

#define LS_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* The only compiler extensions this project uses are branch hints; a compiler
 * without __builtin_expect still gets correct (unhinted) code. Vendor
 * attributes such as [[gnu::cold]] and [[gnu::format]] need no guard: they are
 * standard C23 attribute syntax with an implementation-defined namespace. */
#if defined(__GNUC__)
#define LS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LS_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define LS_UNLIKELY(x) (!!(x))
#define LS_LIKELY(x) (!!(x))
#endif

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

#endif /* LINKSTAY_BASE_H */
