#ifndef LINKSTAY_LOG_H
#define LINKSTAY_LOG_H

/*
 * log.h — leveled logging to stderr.
 *
 * The hot-path loggers (info/debug) are inline and gated on the configured
 * level so filtered messages cost nothing beyond a comparison. The cold
 * loggers (error/warn) carry the `cold` attribute since they fire rarely.
 */

#include "base.h"

#include <stdarg.h>

#define LS_LOG_BUFFER_SIZE 2048U

typedef enum {
  LS_LOG_SILENT = -1, /* completely silent */
  LS_LOG_ERROR = 0,
  LS_LOG_WARN = 1,
  LS_LOG_INFO = 2, /* default */
  LS_LOG_DEBUG = 3 /* verbose: prints per-ping latency */
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

/* ls_error / ls_warn are marked cold since they fire rarely. The level check
 * avoids all formatting cost when the message is filtered. A null log is
 * silently ignored so callers need not guard every call. */
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

#endif /* LINKSTAY_LOG_H */
