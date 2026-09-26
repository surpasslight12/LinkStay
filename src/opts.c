/*
 * opts.c — option resolution and validation. One table (OPT_SPECS) describes
 * every option and drives getopt_long, the environment layer, and error naming.
 * Validation is limited to what can be decided without I/O beyond reading the
 * environment and probing for the systemd runtime.
 */

#include "opts.h"

#include "addr.h"

#include <errno.h>
#include <getopt.h>
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
#define LS_DEFAULT_POWEROFF false
#define LS_DEFAULT_SYSTEMD true

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

/* ---- Per-option value appliers ----
 *
 * Every applier has the same shape so the option table below can drive both
 * the CLI and the environment layer. `name` is the user-facing spelling of the
 * source ("--target" or "LINKSTAY_TARGET"); a NULL value means a bare CLI flag
 * (only the boolean options accept that) because environment values are always
 * non-NULL strings. */

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
 * an empty string, and a leading '-' (which wraps around to a huge unsigned
 * value). A leading '+' stays valid because the historical parser took it. */
static bool has_unsigned_shape(const char *value) {
  if (value == nullptr) {
    return false;
  }
  const char *digits = value[0] == '+' ? value + 1 : value;
  return digits[0] >= '0' && digits[0] <= '9';
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
  if (value == nullptr) { /* bare CLI flag, e.g. -p or --systemd */
    *dest = true;
    return true;
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
                    "Invalid value for %s: %s (use a positive integer, "
                    "optionally suffixed 's' or 'ms')",
                    name, display);
}

static bool apply_duration(const char *name, const char *value,
                           uint64_t default_unit_ms, uint64_t max_ms,
                           uint64_t *out_ms, ls_err_t *err) {
  const char *display = value != nullptr ? value : "<empty>";
  if (!has_unsigned_shape(value)) {
    return invalid_duration(name, display, err);
  }
  errno = 0;
  char *endptr = nullptr;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || parsed == 0) {
    return invalid_duration(name, display, err);
  }
  uint64_t unit_ms = default_unit_ms;
  if (*endptr != '\0') {
    if (strcmp(endptr, "s") == 0) {
      unit_ms = LS_MS_PER_SEC;
    } else if (strcmp(endptr, "ms") == 0) {
      unit_ms = 1;
    } else {
      return invalid_duration(name, display, err);
    }
  }
  uint64_t result_ms = 0;
  if (ckd_mul(&result_ms, (uint64_t)parsed, unit_ms)) {
    return ls_err_set(err, "Invalid value for %s: %s (too large)", name,
                      display);
  }
  if (result_ms == 0 || result_ms > max_ms) {
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

/* Typed adapters binding an applier signature to one ls_opts_t field. Keeping
 * them separate (instead of offsets into the struct) keeps the table
 * type-safe; adding an option means adding one row plus its adapter. */
static bool apply_target(const char *name, const char *value, ls_opts_t *opts,
                         ls_err_t *err) {
  return apply_string(name, value, opts->target, sizeof(opts->target), err);
}

static bool apply_interval(const char *name, const char *value, ls_opts_t *opts,
                           ls_err_t *err) {
  return apply_duration(name, value, LS_MS_PER_SEC,
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
  return apply_duration(name, value, 1, (uint64_t)INT_MAX, &opts->timeout_ms,
                        err);
}

static bool apply_poweroff(const char *name, const char *value, ls_opts_t *opts,
                           ls_err_t *err) {
  return apply_bool(name, value, &opts->poweroff, err);
}

static bool apply_systemd(const char *name, const char *value, ls_opts_t *opts,
                          ls_err_t *err) {
  return apply_bool(name, value, &opts->systemd, err);
}

static bool apply_level(const char *name, const char *value, ls_opts_t *opts,
                        ls_err_t *err) {
  return apply_log_level(name, value, &opts->log_level, err);
}

/* ---- Option table (single source of truth) ----
 *
 * One row drives getopt_long (built at run time), the environment layer, and
 * error naming. help/version carry no applier: they are handled by the
 * pre-scan so they win over invalid arguments or environment values. */

typedef enum {
  ARG_NONE,     /* no argument: -v, -h */
  ARG_REQUIRED, /* -t <ip>, --target <ip> */
  ARG_OPTIONAL, /* -p[<bool>], --poweroff[=<bool>] */
} arg_kind_t;

/* Field order keeps the table compact: the pointer trio first, then the two
 * small scalars, so the compiler inserts no padding between them. */
typedef struct {
  const char *long_name; /* without the leading "--" */
  const char *env_name;  /* NULL for CLI-only options */
  apply_fn_t apply;      /* NULL for help/version */
  arg_kind_t arg_kind;
  char short_name;
} opt_spec_t;

static const opt_spec_t OPT_SPECS[] = {
    {"target", "LINKSTAY_TARGET", apply_target, ARG_REQUIRED, 't'},
    {"interval", "LINKSTAY_INTERVAL", apply_interval, ARG_REQUIRED, 'i'},
    {"threshold", "LINKSTAY_THRESHOLD", apply_threshold, ARG_REQUIRED, 'n'},
    {"timeout", "LINKSTAY_TIMEOUT", apply_timeout, ARG_REQUIRED, 'w'},
    {"poweroff", "LINKSTAY_POWEROFF", apply_poweroff, ARG_OPTIONAL, 'p'},
    {"log-level", "LINKSTAY_LOG_LEVEL", apply_level, ARG_REQUIRED, 'l'},
    {"systemd", "LINKSTAY_SYSTEMD", apply_systemd, ARG_OPTIONAL, 's'},
    {"version", nullptr, nullptr, ARG_NONE, 'v'},
    {"help", nullptr, nullptr, ARG_NONE, 'h'},
};

static const opt_spec_t *find_spec(int short_name) {
  for (size_t i = 0; i < LS_ARRAY_LEN(OPT_SPECS); i++) {
    if (OPT_SPECS[i].short_name == short_name) {
      return &OPT_SPECS[i];
    }
  }
  return nullptr;
}

/* getopt_long needs a struct option array plus an optstring; both are derived
 * from OPT_SPECS so the two can never drift apart. */
typedef struct {
  struct option long_opts[LS_ARRAY_LEN(OPT_SPECS) + 1];
  char optstring[LS_ARRAY_LEN(OPT_SPECS) * 3 + 1];
} opt_tables_t;

static void build_getopt_tables(opt_tables_t *tables) {
  size_t pos = 0;
  for (size_t i = 0; i < LS_ARRAY_LEN(OPT_SPECS); i++) {
    const opt_spec_t *spec = &OPT_SPECS[i];
    int has_arg = no_argument;
    if (spec->arg_kind == ARG_REQUIRED) {
      has_arg = required_argument;
    } else if (spec->arg_kind == ARG_OPTIONAL) {
      has_arg = optional_argument;
    }
    tables->long_opts[i] = (struct option){
        .name = spec->long_name,
        .has_arg = has_arg,
        .flag = nullptr,
        .val = spec->short_name,
    };

    tables->optstring[pos++] = spec->short_name;
    if (spec->arg_kind != ARG_NONE) {
      tables->optstring[pos++] = ':';
    }
    if (spec->arg_kind == ARG_OPTIONAL) {
      tables->optstring[pos++] = ':';
    }
  }
  tables->optstring[pos] = '\0';
  tables->long_opts[LS_ARRAY_LEN(OPT_SPECS)] = (struct option){};
}

/* ---- Defaults, environment, CLI ---- */

static void load_defaults(ls_opts_t *opts) {
  *opts = (ls_opts_t){
      .interval_ms = LS_DEFAULT_INTERVAL_MS,
      .fail_threshold = LS_DEFAULT_FAIL_THRESHOLD,
      .timeout_ms = LS_DEFAULT_TIMEOUT_MS,
      .poweroff = LS_DEFAULT_POWEROFF,
      .log_level = LS_LOG_INFO,
      .systemd = LS_DEFAULT_SYSTEMD,
  };
  (void)snprintf(opts->target, sizeof(opts->target), "%s", LS_DEFAULT_TARGET);
}

static bool load_cli(ls_opts_t *opts, int argc, char **argv,
                     const opt_tables_t *tables,
                     bool seen[LS_ARRAY_LEN(OPT_SPECS)], ls_err_t *err) {
  optind = 1;
  opterr = 0;

  int letter;
  while ((letter = getopt_long(argc, argv, tables->optstring, tables->long_opts,
                               nullptr)) != -1) {
    if (letter == '?') {
      /* getopt_long reports both missing arguments and unknown options as
       * '?'. Only a known option whose argument is required turns the first
       * case into a message about the argument. */
      const opt_spec_t *failed = find_spec(optopt);
      if (failed != nullptr && failed->arg_kind == ARG_REQUIRED) {
        return ls_err_set(
            err, "Option requires an argument or has invalid value: -%c",
            optopt);
      }
      return ls_err_set(err, "Unknown option: %s",
                        argv[optind - 1] != nullptr ? argv[optind - 1]
                                                    : "<unknown>");
    }

    const opt_spec_t *spec = find_spec(letter);
    if (spec == nullptr) {
      return ls_err_set(err, "Internal option parse failure");
    }
    size_t index = (size_t)(spec - OPT_SPECS);
    seen[index] = true;
    if (spec->apply == nullptr) {
      continue; /* help/version: already handled by the pre-scan */
    }

    char display[32];
    (void)snprintf(display, sizeof(display), "--%s", spec->long_name);
    if (!spec->apply(display, optarg, opts, err)) {
      return false;
    }
  }

  if (optind < argc) {
    return ls_err_set(err, "Unexpected argument: %s", argv[optind]);
  }
  return true;
}

/* Applies every environment variable whose CLI counterpart was not given, so a
 * valid (or invalid) env value can never defeat an explicit CLI option. */
static bool load_env(ls_opts_t *opts,
                     const bool seen[LS_ARRAY_LEN(OPT_SPECS)], ls_err_t *err) {
  for (size_t i = 0; i < LS_ARRAY_LEN(OPT_SPECS); i++) {
    const opt_spec_t *spec = &OPT_SPECS[i];
    if (seen[i] || spec->env_name == nullptr || spec->apply == nullptr) {
      continue;
    }
    const char *value = getenv(spec->env_name);
    if (value != nullptr && !spec->apply(spec->env_name, value, opts, err)) {
      return false;
    }
  }
  return true;
}

/* ---- Cross-field validation ---- */

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

/* ---- Help & version ---- */

/* Hand-written user-facing prose; keep the option spellings in sync with
 * OPT_SPECS when adding or renaming an option. */
static void print_usage(void) {
  char interval_default[32];
  char timeout_default[32];
  ls_format_duration(LS_DEFAULT_INTERVAL_MS, interval_default,
                     sizeof(interval_default));
  ls_format_duration(LS_DEFAULT_TIMEOUT_MS, timeout_default,
                     sizeof(timeout_default));

  printf("Usage: %s [options]\n\n", LS_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: %s)\n",
         LS_DEFAULT_TARGET);
  printf("  -i, --interval <n[s|ms]>    Probe interval; a plain number\n");
  printf("                              means seconds, 's'/'ms' suffixes "
         "accepted\n");
  printf("                              (default: %s)\n", interval_default);
  printf("  -n, --threshold <num>       Consecutive failures before shutdown "
         "(default: %d)\n",
         LS_DEFAULT_FAIL_THRESHOLD);
  printf("  -w, --timeout <n[s|ms]>     Reply timeout; a plain number means\n");
  printf("                              milliseconds, 's'/'ms' suffixes "
         "accepted;\n");
  printf("                              must be smaller than --interval\n");
  printf("                              (default: %s)\n\n", timeout_default);
  printf("Shutdown Options:\n");
  printf("  -p[ARG], --poweroff[=ARG]   Actually power the system off when "
         "the threshold is reached\n");
  printf("                              (default: %s; false only simulates)\n",
         LS_DEFAULT_POWEROFF ? "true" : "false");
  printf("                              Flag alone enables it; use "
         "--poweroff=false\n");
  printf("                              or -pfalse to disable\n");
  printf("                              true requires a systemd host with "
         "%s\n\n",
         LS_SYSTEMCTL_PATH);
  printf("Logging Options:\n");
  printf("  -l, --log-level <level>     Log level: %s\n",
         LS_LOG_LEVEL_VALUES);
  printf("                              (default: info)\n");
  printf("System Integration:\n");
  printf("  -s[ARG], --systemd[=ARG]    Enable/disable systemd integration "
         "(default: %s)\n",
         LS_DEFAULT_SYSTEMD ? "true" : "false");
  printf("                              Flag alone enables it; use "
         "--systemd=false or -sfalse to disable\n");
  printf("                              Watchdog is auto-enabled with "
         "systemd\n");
  printf("                              Log timestamps are auto-disabled when "
         "systemd is enabled\n");
  printf("                              Otherwise timestamps stay enabled\n");
  printf("                              Accepted values: %s\n\n",
         LS_BOOL_VALUES);
  printf("General Options:\n");
  printf("  -v, --version               Show version information\n");
  printf("  -h, --help                  Show this help message\n\n");
  printf("Environment Variables (lower priority than CLI args):\n");
  printf("  Network:      LINKSTAY_TARGET, LINKSTAY_INTERVAL,\n");
  printf("                LINKSTAY_THRESHOLD, LINKSTAY_TIMEOUT\n");
  printf("  Shutdown:     LINKSTAY_POWEROFF\n");
  printf("  Logging:      LINKSTAY_LOG_LEVEL\n");
  printf("  Integration:  LINKSTAY_SYSTEMD\n");
  printf("\nExamples:\n");
  printf("  %s -t 1.1.1.1 -i 10 -n 5\n", LS_PROGRAM_NAME);
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --poweroff\n", LS_PROGRAM_NAME);
  printf("  %s -t 8.8.8.8 -l debug --systemd=false\n", LS_PROGRAM_NAME);
  printf("  %s -t 8.8.8.8 -i 5 -n 3 -p -sfalse -l debug\n", LS_PROGRAM_NAME);
}

static void print_version(void) {
  printf("%s version %s\n", LS_PROGRAM_NAME, LS_VERSION);
  printf("%s network monitor\n", LS_PROGRAM_NAME);
}

static bool handle_exit_option(int requested, bool *exit_requested) {
  if (requested == 'v') {
    print_version();
    *exit_requested = true;
    return true;
  }
  if (requested == 'h') {
    print_usage();
    *exit_requested = true;
    return true;
  }
  return false;
}

/* Runs getopt once with the real option table solely to detect --help or
 * --version. This lets those flags win even when the environment or other
 * arguments are invalid. getopt globals are restored so the real parse starts
 * clean. */
static int scan_exit_option(int argc, char **argv,
                            const opt_tables_t *tables) {
  int saved_optind = optind;
  int saved_opterr = opterr;
  int saved_optopt = optopt;
  char *saved_optarg = optarg;
  optind = 1;
  opterr = 0;

  int requested = 0;
  int letter;
  while ((letter = getopt_long(argc, argv, tables->optstring, tables->long_opts,
                               nullptr)) != -1) {
    if (letter == 'v' || letter == 'h') {
      requested = letter;
      break;
    }
  }

  optind = saved_optind;
  opterr = saved_opterr;
  optopt = saved_optopt;
  optarg = saved_optarg;
  return requested;
}

/* ---- Public API ---- */

bool ls_opts_timestamps(const ls_opts_t *restrict opts) {
  return opts != nullptr && !opts->systemd;
}

void ls_opts_dump(const ls_opts_t *restrict opts,
                  const ls_log_t *restrict log) {
  if (opts == nullptr || log == nullptr) {
    return;
  }
  char interval_str[32];
  char timeout_str[32];
  ls_format_duration(opts->interval_ms, interval_str, sizeof(interval_str));
  ls_format_duration(opts->timeout_ms, timeout_str, sizeof(timeout_str));
  ls_debug(log, "Configuration:");
  ls_debug(log, "  Target: %s", opts->target);
  ls_debug(log, "  Interval: %s", interval_str);
  ls_debug(log, "  Threshold: %d", opts->fail_threshold);
  ls_debug(log, "  Timeout: %s", timeout_str);
  ls_debug(log, "  Poweroff: %s", opts->poweroff ? "true" : "false");
  ls_debug(log, "  Log Level: %s", ls_log_level_name(opts->log_level));
  ls_debug(log, "  Timestamp: %s",
           ls_opts_timestamps(opts) ? "true" : "false");
  ls_debug(log, "  Systemd: %s", opts->systemd ? "true" : "false");
}

bool ls_opts_resolve(ls_opts_t *restrict opts, int argc, char **restrict argv,
                     bool *restrict exit_requested, ls_err_t *restrict err) {
  if (opts == nullptr || argv == nullptr || exit_requested == nullptr) {
    return false;
  }

  *exit_requested = false;
  load_defaults(opts);

  opt_tables_t tables;
  build_getopt_tables(&tables);

  if (handle_exit_option(scan_exit_option(argc, argv, &tables),
                         exit_requested)) {
    return true;
  }

  bool seen[LS_ARRAY_LEN(OPT_SPECS)] = {false};
  return load_cli(opts, argc, argv, &tables, seen, err) &&
         load_env(opts, seen, err) && validate(opts, err);
}
