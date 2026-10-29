/*
 * opts.c — environment configuration and validation.
 *
 * linkstay has no command line. Defaults are applied first, then every
 * LINKSTAY_* variable present in the environment, then cross-field validation.
 * Validation is limited to what can be decided without I/O beyond reading the
 * environment and probing for the systemd runtime.
 */

#include "opts.h"

#include "addr.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_DEFAULT_TARGET "1.1.1.1"
#define LS_DEFAULT_INTERVAL_MS UINT64_C(10000)
#define LS_DEFAULT_FAIL_THRESHOLD 5
#define LS_DEFAULT_TIMEOUT_MS UINT64_C(3000)
#define LS_DEFAULT_START_DELAY_MS UINT64_C(0)
#define LS_DEFAULT_POWEROFF false

#define LS_BOOL_VALUES "true|false"
#define LS_LOG_LEVEL_VALUES "silent|error|warn|info|debug"
#define LS_SYSTEMD_RUNTIME_DIR "/run/systemd/system"

/* ---- Small named-value parsers ---- */

typedef struct {
  const char *name;
  int value;
} named_value_t;

static const named_value_t LOG_LEVEL_NAMES[] = {
    {"silent", LS_LOG_SILENT}, {"error", LS_LOG_ERROR},
    {"warn", LS_LOG_WARN},     {"info", LS_LOG_INFO},
    {"debug", LS_LOG_DEBUG},
};

static bool lookup_named(const named_value_t *table, size_t count,
                         const char *arg, int *out_value) {
  if (arg == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(arg, table[i].name) == 0) {
      *out_value = table[i].value;
      return true;
    }
  }
  return false;
}

static bool invalid_value(const char *name, const char *value,
                          const char *allowed, ls_err_t *err) {
  return ls_err_set(err, "Invalid value for %s: %s (use %s)", name,
                    value != nullptr ? value : "<empty>", allowed);
}

/* ---- Per-setting appliers ---- */

typedef bool (*apply_fn_t)(const char *name, const char *value,
                           ls_opts_t *opts, ls_err_t *err);

static bool apply_string(const char *name, const char *value, char *dest,
                         size_t size, ls_err_t *err) {
  if (value == nullptr) {
    return invalid_value(name, value, "a string", err);
  }
  int written = snprintf(dest, size, "%s", value);
  if (written < 0 || (size_t)written >= size) {
    return ls_err_set(err, "%s is too long (max %zu characters)", name,
                      size - 1);
  }
  return true;
}

/* Rejects what strtoull() would otherwise accept silently: leading whitespace,
 * an empty string, a leading '+', and a leading '-' (which would wrap around to
 * a huge unsigned value). */
static bool has_unsigned_shape(const char *value) {
  return value != nullptr && value[0] >= '0' && value[0] <= '9';
}

static bool apply_positive_int(const char *name, const char *value,
                               const char *unit, int *dest, ls_err_t *err) {
  if (!has_unsigned_shape(value)) {
    return invalid_value(name, value, "a positive integer", err);
  }
  errno = 0;
  char *endptr = nullptr;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || *endptr != '\0' || parsed < 1 ||
      parsed > (unsigned long long)INT_MAX) {
    return ls_err_set(err, "Invalid value for %s: %s (range 1..%d %s)", name,
                      value, INT_MAX, unit);
  }
  *dest = (int)parsed;
  return true;
}

/* Boolean values are deliberately limited to true/false (case-insensitive):
 * one spelling per state, no numeric or shell-style aliases. */
static bool apply_bool(const char *name, const char *value, bool *dest,
                       ls_err_t *err) {
  if (value == nullptr) {
    return invalid_value(name, value, LS_BOOL_VALUES, err);
  }
  if (strcasecmp(value, "true") == 0) {
    *dest = true;
    return true;
  }
  if (strcasecmp(value, "false") == 0) {
    *dest = false;
    return true;
  }
  return invalid_value(name, value, LS_BOOL_VALUES, err);
}

static bool invalid_duration(const char *name, const char *display,
                             ls_err_t *err) {
  return ls_err_set(err,
                    "Invalid value for %s: %s (use a positive integer "
                    "suffixed 's' or 'ms')",
                    name, display);
}

static bool apply_duration(const char *name, const char *value, bool allow_zero,
                           uint64_t max_ms, uint64_t *out_ms, ls_err_t *err) {
  const char *display = value != nullptr ? value : "<empty>";
  if (!has_unsigned_shape(value)) {
    return invalid_duration(name, display, err);
  }
  errno = 0;
  char *endptr = nullptr;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || (parsed == 0 && !allow_zero)) {
    return invalid_duration(name, display, err);
  }
  uint64_t unit_ms = 0;
  if (strcmp(endptr, "s") == 0) {
    unit_ms = LS_MS_PER_SEC;
  } else if (strcmp(endptr, "ms") == 0) {
    unit_ms = 1;
  } else {
    return invalid_duration(name, display, err);
  }
  uint64_t result_ms = 0;
  if (ckd_mul(&result_ms, (uint64_t)parsed, unit_ms)) {
    return ls_err_set(err, "Invalid value for %s: %s (too large)", name,
                      display);
  }
  if ((result_ms == 0 && !allow_zero) || result_ms > max_ms) {
    return ls_err_set(err, "Invalid value for %s: %s (range 1..%" PRIu64
                           " ms)",
                      name, display, max_ms);
  }
  *out_ms = result_ms;
  return true;
}

static bool apply_log_level(const char *name, const char *value,
                            ls_log_level_t *dest, ls_err_t *err) {
  int parsed = 0;
  if (!lookup_named(LOG_LEVEL_NAMES, LS_ARRAY_LEN(LOG_LEVEL_NAMES), value,
                    &parsed)) {
    return invalid_value(name, value, LS_LOG_LEVEL_VALUES, err);
  }
  *dest = (ls_log_level_t)parsed;
  return true;
}

/* Typed adapters binding an applier signature to one ls_opts_t field. */
static bool apply_target(const char *name, const char *value, ls_opts_t *opts,
                         ls_err_t *err) {
  return apply_string(name, value, opts->target, sizeof(opts->target), err);
}

static bool apply_interval(const char *name, const char *value, ls_opts_t *opts,
                           ls_err_t *err) {
  return apply_duration(name, value, false,
                        (uint64_t)INT_MAX * LS_MS_PER_SEC, &opts->interval_ms,
                        err);
}

static bool apply_threshold(const char *name, const char *value,
                            ls_opts_t *opts, ls_err_t *err) {
  return apply_positive_int(name, value, "failures", &opts->fail_threshold,
                            err);
}

static bool apply_timeout(const char *name, const char *value, ls_opts_t *opts,
                          ls_err_t *err) {
  return apply_duration(name, value, false, (uint64_t)INT_MAX,
                        &opts->timeout_ms, err);
}

static bool apply_start_delay(const char *name, const char *value,
                              ls_opts_t *opts, ls_err_t *err) {
  return apply_duration(name, value, true, (uint64_t)INT_MAX,
                        &opts->start_delay_ms, err);
}

static bool apply_poweroff(const char *name, const char *value, ls_opts_t *opts,
                           ls_err_t *err) {
  return apply_bool(name, value, &opts->poweroff, err);
}

static bool apply_level(const char *name, const char *value, ls_opts_t *opts,
                        ls_err_t *err) {
  return apply_log_level(name, value, &opts->log_level, err);
}

/* ---- Environment table ---- */

typedef struct {
  const char *env_name;
  apply_fn_t apply;
} env_spec_t;

static const env_spec_t ENV_SPECS[] = {
    {"LINKSTAY_TARGET", apply_target},
    {"LINKSTAY_INTERVAL", apply_interval},
    {"LINKSTAY_THRESHOLD", apply_threshold},
    {"LINKSTAY_TIMEOUT", apply_timeout},
    {"LINKSTAY_START_DELAY", apply_start_delay},
    {"LINKSTAY_POWEROFF", apply_poweroff},
    {"LINKSTAY_LOG_LEVEL", apply_level},
};

/* ---- Defaults, environment, validation ---- */

static void load_defaults(ls_opts_t *opts) {
  *opts = (ls_opts_t){
      .target = LS_DEFAULT_TARGET,
      .interval_ms = LS_DEFAULT_INTERVAL_MS,
      .fail_threshold = LS_DEFAULT_FAIL_THRESHOLD,
      .timeout_ms = LS_DEFAULT_TIMEOUT_MS,
      .start_delay_ms = LS_DEFAULT_START_DELAY_MS,
      .poweroff = LS_DEFAULT_POWEROFF,
      .log_level = LS_LOG_INFO,
  };
}

static bool load_env(ls_opts_t *opts, ls_err_t *err) {
  for (size_t i = 0; i < LS_ARRAY_LEN(ENV_SPECS); i++) {
    const env_spec_t *spec = &ENV_SPECS[i];
    const char *value = getenv(spec->env_name);
    if (value != nullptr &&
        !spec->apply(spec->env_name, value, opts, err)) {
      return false;
    }
  }
  return true;
}

static bool systemd_runtime_available(void) {
  struct stat runtime_dir_stat;
  return access(LS_SYSTEMCTL_PATH, X_OK) == 0 &&
         stat(LS_SYSTEMD_RUNTIME_DIR, &runtime_dir_stat) == 0 &&
         S_ISDIR(runtime_dir_stat.st_mode);
}

static bool validate(const ls_opts_t *opts, ls_err_t *err) {
  if (opts->target[0] == '\0') {
    return ls_err_set(err, "Target host cannot be empty");
  }
  /* Literal syntax is owned by addr.c; the failure message stays here because
   * this is the layer that documents the DNS-disabled contract. */
  if (!ls_addr_is_literal(opts->target)) {
    return ls_err_set(
        err, "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }

  /* Both durations are already in milliseconds; no overflow possible. */
  if (opts->timeout_ms >= opts->interval_ms) {
    return ls_err_set(err,
                      "Timeout (%" PRIu64 " ms) must be smaller than "
                      "interval (%" PRIu64 " ms)",
                      opts->timeout_ms, opts->interval_ms);
  }

  if (opts->poweroff && !systemd_runtime_available()) {
    return ls_err_set(
        err, "poweroff requires a systemd host with %s and %s available",
        LS_SYSTEMCTL_PATH, LS_SYSTEMD_RUNTIME_DIR);
  }
  return true;
}

void ls_opts_dump(const ls_opts_t *restrict opts,
                  const ls_log_t *restrict log) {
  if (opts == nullptr || log == nullptr) {
    return;
  }
  char interval_str[32];
  char timeout_str[32];
  char start_delay_str[32];
  ls_format_duration(opts->interval_ms, interval_str, sizeof(interval_str));
  ls_format_duration(opts->timeout_ms, timeout_str, sizeof(timeout_str));
  ls_format_duration(opts->start_delay_ms, start_delay_str,
                     sizeof(start_delay_str));
  ls_debug(log, "Configuration:");
  ls_debug(log, "  Target: %s", opts->target);
  ls_debug(log, "  Interval: %s", interval_str);
  ls_debug(log, "  Threshold: %d", opts->fail_threshold);
  ls_debug(log, "  Timeout: %s", timeout_str);
  ls_debug(log, "  Start Delay: %s", start_delay_str);
  ls_debug(log, "  Poweroff: %s", opts->poweroff ? "true" : "false");
  ls_debug(log, "  Log Level: %s", ls_log_level_name(opts->log_level));
}

bool ls_opts_resolve(ls_opts_t *restrict opts, ls_err_t *restrict err) {
  if (opts == nullptr) {
    return false;
  }
  load_defaults(opts);
  return load_env(opts, err) && validate(opts, err);
}
