/*
 * log.c — leveled logging to stderr, one fprintf per emitted line. journald
 * adds the timestamp, so lines carry only the level and message.
 */

#include "log.h"

#include <stdio.h>

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

void ls_log_init(ls_log_t *restrict log, ls_log_level_t level) {
  if (log == nullptr) {
    return;
  }
  log->level = level;
}

void ls_log_va(const ls_log_t *restrict log, ls_log_level_t level,
               const char *restrict fmt, va_list ap) {
  if (LS_UNLIKELY(log == nullptr || fmt == nullptr ||
                  log->level == LS_LOG_SILENT || level > log->level)) {
    return;
  }

  char message[LS_LOG_BUFFER_SIZE];
  (void)vsnprintf(message, sizeof(message), fmt, ap);
  fprintf(stderr, "[%s] %s\n", ls_log_level_name(level), message);
}
