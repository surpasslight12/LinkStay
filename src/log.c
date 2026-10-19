/*
 * log.c — leveled logging to stderr, one fprintf per emitted line.
 */

#include "log.h"

#include <stdio.h>
#include <time.h>

/* Indexed by level - LS_LOG_SILENT so the table order mirrors the enum. */
static const char *const LEVEL_NAMES[] = {
    "SILENT", "ERROR", "WARN", "INFO", "DEBUG",
};

const char *ls_log_level_name(ls_log_level_t level) {
  int index = (int)level - (int)LS_LOG_SILENT;
  if (index < 0 || (size_t)index >= LS_ARRAY_LEN(LEVEL_NAMES)) {
    return "UNKNOWN";
  }
  return LEVEL_NAMES[index];
}

void ls_log_init(ls_log_t *restrict log, ls_log_level_t level,
                 bool timestamps) {
  if (log == nullptr) {
    return;
  }
  log->level = level;
  log->timestamps = timestamps;
}

/* Wall-clock prefix; empty when the clock or the timezone conversion fails. */
static void format_timestamp(char *buffer, size_t size) {
  struct timespec ts;
  struct tm tm_info;
  if (LS_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
                  localtime_r(&ts.tv_sec, &tm_info) == nullptr)) {
    buffer[0] = '\0';
    return;
  }
  (void)snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                 ts.tv_nsec / (long)LS_NS_PER_MS);
}

void ls_log_va(const ls_log_t *restrict log, ls_log_level_t level,
               const char *restrict fmt, va_list ap) {
  if (LS_UNLIKELY(log == nullptr || fmt == nullptr ||
                  log->level == LS_LOG_SILENT || level > log->level)) {
    return;
  }

  char message[LS_LOG_BUFFER_SIZE];
  (void)vsnprintf(message, sizeof(message), fmt, ap);

  if (log->timestamps) {
    char timestamp[64];
    format_timestamp(timestamp, sizeof(timestamp));
    fprintf(stderr, "[%s] [%s] %s\n", timestamp, ls_log_level_name(level),
            message);
  } else {
    fprintf(stderr, "[%s] %s\n", ls_log_level_name(level), message);
  }
}
