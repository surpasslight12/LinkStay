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
 * Project identity, shared macros, the unified error type, monotonic clock,
 * and leveled logging. Every other module includes this header.
 */

#include <stdarg.h>
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

/* ---- Leveled logging ---- */

#define LS_LOG_BUFFER_SIZE 2048U

typedef enum {
  LS_LOG_SILENT = -1,
  LS_LOG_ERROR = 0,
  LS_LOG_WARN = 1,
  LS_LOG_INFO = 2,
  LS_LOG_DEBUG = 3,
} ls_log_level_t;

typedef struct {
  ls_log_level_t level;
  bool timestamps;
} ls_log_t;

void ls_log_init(ls_log_t *restrict log, ls_log_level_t level,
                 bool timestamps);
void ls_log_va(const ls_log_t *restrict log, ls_log_level_t level,
               const char *restrict fmt, va_list ap);
const char *ls_log_level_name(ls_log_level_t level);

#define LS_DEFINE_LOGGER(name, lvl, attrs)                                     \
  [[gnu::format(printf, 2, 3)]] attrs                                          \
  static inline void name(const ls_log_t *restrict log,                       \
                          const char *restrict fmt, ...);                      \
  [[gnu::format(printf, 2, 3)]] attrs                                          \
  static inline void name(const ls_log_t *restrict log,                       \
                          const char *restrict fmt, ...) {                     \
    if (LS_LIKELY(log != nullptr) && log->level >= (lvl)) {                    \
      va_list args;                                                            \
      va_start(args, fmt);                                                     \
      ls_log_va(log, (lvl), fmt, args);                                        \
      va_end(args);                                                            \
    }                                                                          \
  }

LS_DEFINE_LOGGER(ls_error, LS_LOG_ERROR, LS_COLD)
LS_DEFINE_LOGGER(ls_warn, LS_LOG_WARN, LS_COLD)
LS_DEFINE_LOGGER(ls_info, LS_LOG_INFO, )
LS_DEFINE_LOGGER(ls_debug, LS_LOG_DEBUG, )

#endif /* LINKSTAY_BASE_H */
