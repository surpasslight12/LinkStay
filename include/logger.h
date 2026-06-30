#ifndef LINKSTAY_LOGGER_H
#define LINKSTAY_LOGGER_H

/*
 * logger.h — leveled logging to stderr.
 *
 * The hot-path loggers (info/debug) are inline and gated on the configured
 * level so filtered messages cost nothing beyond a comparison. The cold
 * loggers (error/warn) carry the `cold` attribute since they fire rarely.
 */

#include "common.h"

#include <stdarg.h>

#define LINKSTAY_LOG_BUFFER_SIZE 2048U

typedef enum {
  LOG_LEVEL_SILENT = -1, /* completely silent, suitable for systemd */
  LOG_LEVEL_ERROR = 0,
  LOG_LEVEL_WARN = 1,
  LOG_LEVEL_INFO = 2, /* default */
  LOG_LEVEL_DEBUG = 3 /* verbose: prints per-ping latency */
} log_level_t;

typedef struct {
  log_level_t level;
  bool enable_timestamp;
} logger_t;

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp);
void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap);
const char *log_level_to_string(log_level_t level);

/* logger_error / logger_warn are marked cold since they fire rarely.
   The level check avoids any formatting cost when the level is filtered.
   NULL logger is silently ignored so callers need not guard every call. */
#define DEFINE_LOGGER(name, lvl, attrs)                                        \
  static inline void attrs name(const logger_t *restrict logger,               \
                                const char *restrict fmt, ...)                 \
      __attribute__((format(printf, 2, 3)));                                   \
  static inline void attrs name(const logger_t *restrict logger,               \
                                const char *restrict fmt, ...) {               \
    if (LINKSTAY_LIKELY(logger != NULL) && logger->level >= (lvl)) {           \
      va_list args;                                                            \
      va_start(args, fmt);                                                     \
      logger_log_va(logger, (lvl), fmt, args);                                 \
      va_end(args);                                                            \
    }                                                                          \
  }

DEFINE_LOGGER(logger_error, LOG_LEVEL_ERROR, LINKSTAY_COLD)
DEFINE_LOGGER(logger_warn, LOG_LEVEL_WARN, LINKSTAY_COLD)
DEFINE_LOGGER(logger_info, LOG_LEVEL_INFO, )
DEFINE_LOGGER(logger_debug, LOG_LEVEL_DEBUG, )

#endif /* LINKSTAY_LOGGER_H */
