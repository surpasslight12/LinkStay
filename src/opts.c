#include "opts.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_DEFAULT_TARGET "1.1.1.1"
#define LS_DEFAULT_INTERVAL_SEC 10
#define LS_DEFAULT_FAIL_THRESHOLD 5
#define LS_DEFAULT_TIMEOUT_MS 3000
#define LS_DEFAULT_POWEROFF false
#define LS_DEFAULT_SYSTEMD true

#define LS_BOOL_VALUES "true|false|1|0|yes|no|on|off"
#define LS_LOG_LEVEL_VALUES "silent|error|warn|info|debug"
#define LS_SYSTEMCTL_PATH "/usr/bin/systemctl"
#define LS_SYSTEMD_RUNTIME_DIR "/run/systemd/system"

#define OPTSTRING "t:i:n:w:p::l:s::vh"

/* ---- Small named-value parsers ---- */

typedef struct {
  const char *name;
  int value;
} named_value_t;

static const named_value_t BOOL_NAMES[] = {
    {"true", 1},  {"1", 1}, {"yes", 1}, {"on", 1},
    {"false", 0}, {"0", 0}, {"no", 0},  {"off", 0},
};

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

static bool apply_positive_int(const char *name, const char *value,
                               const char *unit, int *dest, ls_err_t *err) {
  if (value == nullptr) {
    return invalid_value(name, value, "a positive integer", err);
  }
  errno = 0;
  char *endptr = nullptr;
  long long parsed = strtoll(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0' || parsed < 1 ||
      parsed > INT_MAX) {
    return ls_err_set(err, "Invalid value for %s: %s (range 1..%d %s)", name,
                      value, INT_MAX, unit);
  }
  *dest = (int)parsed;
  return true;
}

static bool apply_bool(const char *name, const char *value,
                       bool bare_means_true, bool *dest, ls_err_t *err) {
  if (value == nullptr) {
    if (!bare_means_true) {
      return invalid_value(name, value, LS_BOOL_VALUES, err);
    }
    *dest = true;
    return true;
  }
  int parsed = 0;
  if (!lookup_named(BOOL_NAMES, LS_ARRAY_LEN(BOOL_NAMES), value, &parsed)) {
    return invalid_value(name, value, LS_BOOL_VALUES, err);
  }
  *dest = parsed != 0;
  return true;
}

static bool apply_log_level(const char *name, const char *value,
                            ls_log_level_t *dest, ls_err_t *err) {
  if (value == nullptr) {
    return invalid_value(name, value, LS_LOG_LEVEL_VALUES, err);
  }
  int parsed = 0;
  if (!lookup_named(LOG_LEVEL_NAMES, LS_ARRAY_LEN(LOG_LEVEL_NAMES), value,
                    &parsed)) {
    return invalid_value(name, value, LS_LOG_LEVEL_VALUES, err);
  }
  *dest = (ls_log_level_t)parsed;
  return true;
}

/* ---- Defaults, environment, CLI ---- */

static void load_defaults(ls_opts_t *opts) {
  *opts = (ls_opts_t){
      .interval_sec = LS_DEFAULT_INTERVAL_SEC,
      .fail_threshold = LS_DEFAULT_FAIL_THRESHOLD,
      .timeout_ms = LS_DEFAULT_TIMEOUT_MS,
      .poweroff = LS_DEFAULT_POWEROFF,
      .log_level = LS_LOG_INFO,
      .systemd = LS_DEFAULT_SYSTEMD,
  };
  (void)snprintf(opts->target, sizeof(opts->target), "%s", LS_DEFAULT_TARGET);
}

static const struct option LONG_OPTIONS[] = {
    {"target", required_argument, nullptr, 't'},
    {"interval", required_argument, nullptr, 'i'},
    {"threshold", required_argument, nullptr, 'n'},
    {"timeout", required_argument, nullptr, 'w'},
    {"poweroff", optional_argument, nullptr, 'p'},
    {"log-level", required_argument, nullptr, 'l'},
    {"systemd", optional_argument, nullptr, 's'},
    {"version", no_argument, nullptr, 'v'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0},
};

static bool load_cli(ls_opts_t *opts, int argc, char **argv,
                     bool cli_seen[256], ls_err_t *err) {
  optind = 1;
  opterr = 0;

  int letter;
  while ((letter = getopt_long(argc, argv, OPTSTRING, LONG_OPTIONS, nullptr)) !=
         -1) {
    if (letter == '?') {
      /* getopt_long reports both missing arguments and unknown short options
       * as '?'. A non-zero optopt is a missing required short/long argument
       * only when that letter has a required argument in OPTSTRING. */
      if (optopt != 0 && strchr("tinwl", optopt) != nullptr) {
        return ls_err_set(
            err, "Option requires an argument or has invalid value: -%c",
            optopt);
      }
      return ls_err_set(err, "Unknown option: %s",
                        argv[optind - 1] != nullptr ? argv[optind - 1]
                                                    : "<unknown>");
    }

    cli_seen[(unsigned char)letter] = true;
    switch (letter) {
    case 't':
      if (!apply_string("--target", optarg, opts->target,
                        sizeof(opts->target), err)) {
        return false;
      }
      break;
    case 'i':
      if (!apply_positive_int("--interval", optarg, "seconds",
                              &opts->interval_sec, err)) {
        return false;
      }
      break;
    case 'n':
      if (!apply_positive_int("--threshold", optarg, "failures",
                              &opts->fail_threshold, err)) {
        return false;
      }
      break;
    case 'w':
      if (!apply_positive_int("--timeout", optarg, "milliseconds",
                              &opts->timeout_ms, err)) {
        return false;
      }
      break;
    case 'p':
      if (!apply_bool("--poweroff", optarg, true, &opts->poweroff, err)) {
        return false;
      }
      break;
    case 'l':
      if (!apply_log_level("--log-level", optarg, &opts->log_level, err)) {
        return false;
      }
      break;
    case 's':
      if (!apply_bool("--systemd", optarg, true, &opts->systemd, err)) {
        return false;
      }
      break;
    case 'v':
    case 'h':
      break;
    default:
      return ls_err_set(err, "Internal option parse failure");
    }
  }

  if (optind < argc) {
    return ls_err_set(err, "Unexpected argument: %s", argv[optind]);
  }
  return true;
}

static bool load_env(ls_opts_t *opts, const bool cli_seen[256],
                     ls_err_t *err) {
  const char *value;

  if (!cli_seen[(unsigned char)'t'] &&
      (value = getenv("LINKSTAY_TARGET")) != nullptr &&
      !apply_string("LINKSTAY_TARGET", value, opts->target,
                    sizeof(opts->target), err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'i'] &&
      (value = getenv("LINKSTAY_INTERVAL")) != nullptr &&
      !apply_positive_int("LINKSTAY_INTERVAL", value, "seconds",
                          &opts->interval_sec, err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'n'] &&
      (value = getenv("LINKSTAY_THRESHOLD")) != nullptr &&
      !apply_positive_int("LINKSTAY_THRESHOLD", value, "failures",
                          &opts->fail_threshold, err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'w'] &&
      (value = getenv("LINKSTAY_TIMEOUT")) != nullptr &&
      !apply_positive_int("LINKSTAY_TIMEOUT", value, "milliseconds",
                          &opts->timeout_ms, err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'p'] &&
      (value = getenv("LINKSTAY_POWEROFF")) != nullptr &&
      !apply_bool("LINKSTAY_POWEROFF", value, false, &opts->poweroff, err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'l'] &&
      (value = getenv("LINKSTAY_LOG_LEVEL")) != nullptr &&
      !apply_log_level("LINKSTAY_LOG_LEVEL", value, &opts->log_level, err)) {
    return false;
  }
  if (!cli_seen[(unsigned char)'s'] &&
      (value = getenv("LINKSTAY_SYSTEMD")) != nullptr &&
      !apply_bool("LINKSTAY_SYSTEMD", value, false, &opts->systemd, err)) {
    return false;
  }
  return true;
}

/* ---- Cross-field validation ---- */

static bool is_ip_literal(const char *target) {
  unsigned char addr_buffer[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, target, addr_buffer) == 1 ||
         inet_pton(AF_INET6, target, addr_buffer) == 1;
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
  if (!is_ip_literal(opts->target)) {
    return ls_err_set(
        err, "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }

  /* interval_sec is at most INT_MAX, so multiplying by 1000 cannot overflow
   * uint64_t. */
  uint64_t interval_ms = (uint64_t)opts->interval_sec * LS_MS_PER_SEC;
  if ((uint64_t)opts->timeout_ms >= interval_ms) {
    return ls_err_set(
        err,
        "Timeout (%d ms) must be smaller than interval (%d s = %" PRIu64
        " ms)",
        opts->timeout_ms, opts->interval_sec, interval_ms);
  }

  if (opts->poweroff && !systemd_runtime_available()) {
    return ls_err_set(
        err, "poweroff requires a systemd host with %s and %s available",
        LS_SYSTEMCTL_PATH, LS_SYSTEMD_RUNTIME_DIR);
  }
  return true;
}

/* ---- Help & version ---- */

static void print_usage(void) {
  printf("Usage: %s [options]\n\n", LS_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: %s)\n",
         LS_DEFAULT_TARGET);
  printf("  -i, --interval <sec>        Ping interval in seconds (default: "
         "%d)\n",
         LS_DEFAULT_INTERVAL_SEC);
  printf("  -n, --threshold <num>       Consecutive failures threshold "
         "(default: %d)\n",
         LS_DEFAULT_FAIL_THRESHOLD);
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds; must "
         "be smaller than interval\n");
  printf("                              (default: %d)\n\n",
         LS_DEFAULT_TIMEOUT_MS);
  printf("Shutdown Options:\n");
  printf("  -p[ARG], --poweroff[=ARG]   Actually power the system off when "
         "the threshold is reached\n");
  printf("                              (default: %s; false only simulates)\n",
         LS_DEFAULT_POWEROFF ? "true" : "false");
  printf("                              Flag alone enables it; use "
         "--poweroff=false or -p0 to disable\n");
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
         "--systemd=false, --systemd=0,\n");
  printf("                              -sfalse, or -s0 to disable\n");
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
  printf("  %s -t 8.8.8.8 -l debug --systemd=0\n", LS_PROGRAM_NAME);
  printf("  %s -t 8.8.8.8 -i 5 -n 3 -p -s0 -l debug\n", LS_PROGRAM_NAME);
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
static int scan_exit_option(int argc, char **argv) {
  int saved_optind = optind;
  int saved_opterr = opterr;
  int saved_optopt = optopt;
  char *saved_optarg = optarg;
  optind = 1;
  opterr = 0;

  int requested = 0;
  int letter;
  while ((letter = getopt_long(argc, argv, OPTSTRING, LONG_OPTIONS, nullptr)) !=
         -1) {
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
  ls_debug(log, "Configuration:");
  ls_debug(log, "  Target: %s", opts->target);
  ls_debug(log, "  Interval: %d seconds", opts->interval_sec);
  ls_debug(log, "  Threshold: %d", opts->fail_threshold);
  ls_debug(log, "  Timeout: %d ms", opts->timeout_ms);
  ls_debug(log, "  Poweroff: %s", opts->poweroff ? "true" : "false");
  ls_debug(log, "  Log Level: %s", ls_log_level_name(opts->log_level));
  ls_debug(log, "  Timestamp: %s", ls_opts_timestamps(opts) ? "true" : "false");
  ls_debug(log, "  Systemd: %s", opts->systemd ? "true" : "false");
}

bool ls_opts_resolve(ls_opts_t *restrict opts, int argc, char **restrict argv,
                     bool *restrict exit_requested, ls_err_t *restrict err) {
  if (opts == nullptr || argv == nullptr || exit_requested == nullptr) {
    return false;
  }

  *exit_requested = false;
  load_defaults(opts);

  if (handle_exit_option(scan_exit_option(argc, argv), exit_requested)) {
    return true;
  }

  bool cli_seen[256] = {false};
  if (!load_cli(opts, argc, argv, cli_seen, err) ||
      !load_env(opts, cli_seen, err)) {
    return false;
  }

  return validate(opts, err);
}
