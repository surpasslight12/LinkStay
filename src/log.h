#ifndef LINKSTAY_LOG_H
#define LINKSTAY_LOG_H

/*
 * log.h — leveled logging to stderr for journald.
 *
 * The only module in the project that writes log output. Callers own an
 * ls_log_t and pass it by pointer; the per-level inline wrappers compare the
 * configured level before touching varargs, so a disabled line costs one
 * comparison at the call site. Lines carry no timestamp: journald adds one.
 */

#include "base.h"

#include <stdarg.h>

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
} ls_log_t;

/* Cold-path marker for the error and warning loggers. */
#define LS_COLD [[gnu::cold]]

void ls_log_init(ls_log_t *restrict log, ls_log_level_t level);
void ls_log_va(const ls_log_t *restrict log, ls_log_level_t level,
               const char *restrict fmt, va_list ap);
const char *ls_log_level_name(ls_log_level_t level);

#define LS_DEFINE_LOGGER(name, lvl, attrs)                                     \
  [[gnu::format(printf, 2, 3)]] attrs                                          \
  static inline void name(const ls_log_t *restrict log,                        \
                          const char *restrict fmt, ...);                      \
  [[gnu::format(printf, 2, 3)]] attrs                                          \
  static inline void name(const ls_log_t *restrict log,                        \
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

#endif /* LINKSTAY_LOG_H */
