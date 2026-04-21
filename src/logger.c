#include "linkstay.h"

#include <stdio.h>

/* ---- Timing utilities ---- */

uint64_t get_monotonic_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
  if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0)) {
    if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
      return UINT64_MAX;
    }
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
  uint64_t nsec_ms = (uint64_t)ts.tv_nsec / UINT64_C(1000000);
  uint64_t timestamp = 0;
  if (LINKSTAY_UNLIKELY(ckd_add(&timestamp, seconds_ms, nsec_ms))) {
    return UINT64_MAX;
  }
  return timestamp;
}

static char *get_timestamp_str(char *restrict buffer, size_t size) {
  if (LINKSTAY_UNLIKELY(buffer == NULL || size == 0)) {
    return NULL;
  }
  struct timespec ts;
  if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0)) {
    buffer[0] = '\0';
    return buffer;
  }
  struct tm tm_info;
  if (LINKSTAY_UNLIKELY(localtime_r(&ts.tv_sec, &tm_info) == NULL)) {
    buffer[0] = '\0';
    return buffer;
  }
  snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
           tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
           tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
           ts.tv_nsec / (long)UINT64_C(1000000));
  return buffer;
}

/* ---- Logging ---- */

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp) {
  if (logger == NULL) {
    return;
  }
  logger->level = level;
  logger->enable_timestamp = enable_timestamp;
}

const char *log_level_to_string(log_level_t level) {
  switch (level) {
  case LOG_LEVEL_SILENT:
    return "SILENT";
  case LOG_LEVEL_ERROR:
    return "ERROR";
  case LOG_LEVEL_WARN:
    return "WARN";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  default:
    return "UNKNOWN";
  }
}

static void log_message(const logger_t *restrict logger, log_level_t level,
                        const char *restrict msg) {
  if (LINKSTAY_UNLIKELY(logger == NULL || msg == NULL)) {
    return;
  }
  /* LOG_LEVEL_SILENT suppresses all output, including errors. */
  if (LINKSTAY_UNLIKELY(logger->level == LOG_LEVEL_SILENT)) {
    return;
  }
  /* Suppress messages whose verbosity exceeds the configured level. */
  if (level > logger->level) {
    return;
  }
  char timestamp[64];
  if (logger->enable_timestamp) {
    if (get_timestamp_str(timestamp, sizeof(timestamp)) == NULL) {
      timestamp[0] = '\0';
    }
  }
  const char *level_str = log_level_to_string(level);
  if (logger->enable_timestamp) {
    fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_str, msg);
  } else {
    fprintf(stderr, "[%s] %s\n", level_str, msg);
  }
}

void logger_write(log_level_t level, bool enable_timestamp,
                  const char *restrict fmt, ...) {
  if (fmt == NULL) {
    return;
  }

  logger_t logger;
  logger_init(&logger, level, enable_timestamp);

  va_list args;
  va_start(args, fmt);
  logger_log_va(&logger, level, fmt, args);
  va_end(args);
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap) {
  char buffer[LINKSTAY_LOG_BUFFER_SIZE];
  vsnprintf(buffer, sizeof(buffer), fmt, ap);
  log_message(logger, level, buffer);
}

void log_shutdown_countdown(const logger_t *restrict logger,
                            shutdown_mode_t mode, int delay_minutes) {
  if (logger == NULL) {
    return;
  }

  if (mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_warn(logger, "Starting dry-run countdown for %d minutes",
                delay_minutes);
  } else if (mode == SHUTDOWN_MODE_TRUE_OFF) {
    logger_warn(logger, "Starting true-off countdown for %d minutes",
                delay_minutes);
  }
}
