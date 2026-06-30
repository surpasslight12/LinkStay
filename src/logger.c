#include "logger.h"

#include <stdio.h>

static void logger_format_timestamp(char *restrict buffer, size_t size) {
  if (LINKSTAY_UNLIKELY(buffer == nullptr || size == 0)) {
    return;
  }
  struct timespec ts;
  struct tm tm_info;
  if (LINKSTAY_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
                        localtime_r(&ts.tv_sec, &tm_info) == nullptr)) {
    buffer[0] = '\0';
    return;
  }
  (void)snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                 ts.tv_nsec / (long)UINT64_C(1000000));
}

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp) {
  if (logger == nullptr) {
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
  }
  return "UNKNOWN";
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap) {
  if (LINKSTAY_UNLIKELY(logger == nullptr || fmt == nullptr ||
                        logger->level == LOG_LEVEL_SILENT ||
                        level > logger->level)) {
    return;
  }

  char message[LINKSTAY_LOG_BUFFER_SIZE];
  (void)vsnprintf(message, sizeof(message), fmt, ap);

  if (logger->enable_timestamp) {
    char timestamp[64];
    logger_format_timestamp(timestamp, sizeof(timestamp));
    fprintf(stderr, "[%s] [%s] %s\n", timestamp, log_level_to_string(level),
            message);
  } else {
    fprintf(stderr, "[%s] %s\n", log_level_to_string(level), message);
  }
}
