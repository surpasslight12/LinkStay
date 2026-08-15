#include "base.h"

#include <stdarg.h>
#include <stdio.h>

/* ---- Error type ---- */

bool ls_err_set(ls_err_t *err, const char *fmt, ...) {
  if (err != nullptr) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
    va_end(args);
  }
  return false;
}

/* ---- Monotonic clock ---- */

uint64_t ls_now_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0 &&
                  clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#else
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#endif
  /* CLOCK_MONOTONIC is non-negative on Linux; direct multiplication is
   * safe for any realistic uptime. */
  uint64_t seconds_ms = (uint64_t)ts.tv_sec * LS_MS_PER_SEC;
  return ls_add_sat(seconds_ms, (uint64_t)ts.tv_nsec / UINT64_C(1000000));
}

uint64_t ls_now_ns(void) {
  struct timespec ts;
  if (LS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
  uint64_t seconds_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000);
  return ls_add_sat(seconds_ns, (uint64_t)ts.tv_nsec);
}

/* ---- Logging ---- */

static void format_timestamp(char *restrict buffer, size_t size) {
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
                 ts.tv_nsec / 1000000L);
}

void ls_log_init(ls_log_t *restrict log, ls_log_level_t level,
                 bool timestamps) {
  if (log == nullptr) {
    return;
  }
  log->level = level;
  log->timestamps = timestamps;
}

const char *ls_log_level_name(ls_log_level_t level) {
  switch (level) {
  case LS_LOG_SILENT:
    return "SILENT";
  case LS_LOG_ERROR:
    return "ERROR";
  case LS_LOG_WARN:
    return "WARN";
  case LS_LOG_INFO:
    return "INFO";
  case LS_LOG_DEBUG:
    return "DEBUG";
  }
  return "UNKNOWN";
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
