#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINKSTAY_DEFAULT_TARGET "1.1.1.1"
#define LINKSTAY_DEFAULT_INTERVAL_SEC 10
#define LINKSTAY_DEFAULT_FAIL_THRESHOLD 5
#define LINKSTAY_DEFAULT_TIMEOUT_MS 2000
#define LINKSTAY_DEFAULT_SYSTEMD true
#define LINKSTAY_THRESHOLD_ENV "LINKSTAY_THRESHOLD"
#define LINKSTAY_THRESHOLD_ENV_ALIAS "LINKSTAY_FAIL_THRESHOLD"

#define LINKSTAY_CONFIG_BOOL_VALUES "true|false|1|0|yes|no|on|off"
#define LINKSTAY_CONFIG_LOG_LEVEL_VALUES "silent|error|warn|info|debug"
#define LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES                               \
  "silent|error|warn|info|debug (aliases: none=silent, warning=warn)"
#define LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES "dry-run|true-off|log-only"
#define LINKSTAY_SYSTEMCTL_PATH "/usr/bin/systemctl"
#define LINKSTAY_SYSTEMD_RUNTIME_DIR "/run/systemd/system"

/* ---- Named-value tables ---- */

typedef struct {
  const char *name;
  int value;
} named_value_t;

static const named_value_t BOOL_OPTIONS[] = {
    {"true", 1},  {"1", 1}, {"yes", 1}, {"on", 1},
    {"false", 0}, {"0", 0}, {"no", 0},  {"off", 0},
};

static const named_value_t LOG_LEVEL_OPTIONS[] = {
    {"silent", LOG_LEVEL_SILENT}, {"none", LOG_LEVEL_SILENT},
    {"error", LOG_LEVEL_ERROR},   {"warn", LOG_LEVEL_WARN},
    {"warning", LOG_LEVEL_WARN},  {"info", LOG_LEVEL_INFO},
    {"debug", LOG_LEVEL_DEBUG},
};

static const named_value_t SHUTDOWN_MODE_OPTIONS[] = {
    {"dry-run", SHUTDOWN_MODE_DRY_RUN},
    {"true-off", SHUTDOWN_MODE_TRUE_OFF},
    {"log-only", SHUTDOWN_MODE_LOG_ONLY},
};

static bool parse_named(const named_value_t *options, size_t count,
                        const char *arg, int *out_value) {
  if (arg == NULL) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(arg, options[i].name) == 0) {
      *out_value = options[i].value;
      return true;
    }
  }
  return false;
}

/* ---- getopt configuration ---- */

static const struct option LONG_OPTIONS[] = {
    {"target", required_argument, 0, 't'},
    {"interval", required_argument, 0, 'i'},
    {"threshold", required_argument, 0, 'n'},
    {"fail-threshold", required_argument, 0, 'n'},
    {"timeout", required_argument, 0, 'w'},
    {"mode", required_argument, 0, 'm'},
    {"log-level", required_argument, 0, 'l'},
    {"systemd", optional_argument, 0, 's'},
    {"version", no_argument, 0, 'v'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0},
};

static const char OPTSTRING[] = "t:i:n:w:m:l:s::vh";

/* ---- Small utilities ---- */

__attribute__((format(printf, 3, 4))) static bool
config_errorf(char *error_msg, size_t error_size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(error_msg, error_size, fmt, args);
  va_end(args);
  return false;
}

static bool config_invalid_value(const char *name, const char *value,
                                 const char *allowed, char *error_msg,
                                 size_t error_size) {
  return config_errorf(error_msg, error_size,
                       "Invalid value for %s: %s (use %s)", name,
                       value != NULL ? value : "<empty>", allowed);
}

static bool copy_string(char *dest, size_t dest_size, const char *src,
                        const char *name, char *error_msg, size_t error_size) {
  int written = snprintf(dest, dest_size, "%s", src);
  if (written < 0 || (size_t)written >= dest_size) {
    return config_errorf(error_msg, error_size,
                         "%s is too long (max %zu characters)", name,
                         dest_size - 1);
  }
  return true;
}

static bool parse_positive_int(const char *name, const char *value,
                               const char *unit, int *dest, char *error_msg,
                               size_t error_size) {
  errno = 0;
  char *endptr = NULL;
  long long parsed = strtoll(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0' || parsed < 1 ||
      parsed > INT_MAX) {
    return config_errorf(error_msg, error_size,
                         "Invalid value for %s: %s (range 1..%d %s)", name,
                         value, INT_MAX, unit);
  }
  *dest = (int)parsed;
  return true;
}

static bool parse_bool(const char *arg, bool implicit_true_when_null,
                       bool *out_value) {
  if (arg == NULL) {
    if (!implicit_true_when_null) {
      return false;
    }
    *out_value = true;
    return true;
  }
  int parsed = 0;
  if (!parse_named(BOOL_OPTIONS, LINKSTAY_ARRAY_LEN(BOOL_OPTIONS), arg,
                   &parsed)) {
    return false;
  }
  *out_value = parsed != 0;
  return true;
}

static bool parse_log_level(const char *arg, log_level_t *out_value) {
  int parsed = 0;
  if (!parse_named(LOG_LEVEL_OPTIONS, LINKSTAY_ARRAY_LEN(LOG_LEVEL_OPTIONS),
                   arg, &parsed)) {
    return false;
  }
  *out_value = (log_level_t)parsed;
  return true;
}

static bool parse_shutdown_mode(const char *arg, shutdown_mode_t *out_value) {
  int parsed = 0;
  if (!parse_named(SHUTDOWN_MODE_OPTIONS,
                   LINKSTAY_ARRAY_LEN(SHUTDOWN_MODE_OPTIONS), arg, &parsed)) {
    return false;
  }
  *out_value = (shutdown_mode_t)parsed;
  return true;
}

/* ---- Defaults & validation ---- */

static void config_init_default(config_t *config) {
  *config = (config_t){
      .interval_sec = LINKSTAY_DEFAULT_INTERVAL_SEC,
      .fail_threshold = LINKSTAY_DEFAULT_FAIL_THRESHOLD,
      .timeout_ms = LINKSTAY_DEFAULT_TIMEOUT_MS,
      .shutdown_mode = SHUTDOWN_MODE_DRY_RUN,
      .log_level = LOG_LEVEL_INFO,
      .enable_systemd = LINKSTAY_DEFAULT_SYSTEMD,
  };
  (void)snprintf(config->target, sizeof(config->target), "%s",
                 LINKSTAY_DEFAULT_TARGET);
}

static bool is_ip_literal(const char *target) {
  unsigned char addr_buffer[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, target, addr_buffer) == 1 ||
         inet_pton(AF_INET6, target, addr_buffer) == 1;
}

static bool systemd_runtime_available(void) {
  struct stat runtime_dir_stat;
  return access(LINKSTAY_SYSTEMCTL_PATH, X_OK) == 0 &&
         stat(LINKSTAY_SYSTEMD_RUNTIME_DIR, &runtime_dir_stat) == 0 &&
         S_ISDIR(runtime_dir_stat.st_mode);
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
  for (size_t i = 0; i < LINKSTAY_ARRAY_LEN(SHUTDOWN_MODE_OPTIONS); i++) {
    if (SHUTDOWN_MODE_OPTIONS[i].value == (int)mode) {
      return SHUTDOWN_MODE_OPTIONS[i].name;
    }
  }
  return "unknown";
}

static bool config_validate(const config_t *config, char *error_msg,
                            size_t error_size) {
  if (config->target[0] == '\0') {
    return config_errorf(error_msg, error_size, "Target host cannot be empty");
  }
  if (!is_ip_literal(config->target)) {
    return config_errorf(
        error_msg, error_size,
        "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }

  uint64_t interval_ms = 0;
  if (ckd_mul(&interval_ms, (uint64_t)config->interval_sec,
              LINKSTAY_MS_PER_SEC)) {
    return config_errorf(
        error_msg, error_size,
        "Interval is too large to convert safely to milliseconds");
  }
  if ((uint64_t)config->timeout_ms >= interval_ms) {
    return config_errorf(
        error_msg, error_size,
        "Timeout (%d ms) must be smaller than interval (%d s = %" PRIu64
        " ms) to avoid overlapping probes",
        config->timeout_ms, config->interval_sec, interval_ms);
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_TRUE_OFF &&
      !systemd_runtime_available()) {
    return config_errorf(
        error_msg, error_size,
        "true-off requires a systemd host with %s and %s available",
        LINKSTAY_SYSTEMCTL_PATH, LINKSTAY_SYSTEMD_RUNTIME_DIR);
  }
  return true;
}

bool config_log_timestamps_enabled(const config_t *restrict config) {
  return config != NULL && !config->enable_systemd;
}

void config_print(const config_t *restrict config,
                  const logger_t *restrict logger) {
  if (config == NULL || logger == NULL) {
    return;
  }
  logger_debug(logger, "Configuration:");
  logger_debug(logger, "  Target: %s", config->target);
  logger_debug(logger, "  Interval: %d seconds", config->interval_sec);
  logger_debug(logger, "  Threshold: %d", config->fail_threshold);
  logger_debug(logger, "  Timeout: %d ms", config->timeout_ms);
  logger_debug(logger, "  Shutdown Mode: %s",
               shutdown_mode_to_string(config->shutdown_mode));
  logger_debug(logger, "  Log Level: %s",
               log_level_to_string(config->log_level));
  logger_debug(logger, "  Timestamp: %s",
               config_log_timestamps_enabled(config) ? "true" : "false");
  logger_debug(logger, "  Systemd: %s",
               config->enable_systemd ? "true" : "false");
}

/* ---- Help & version ---- */

static void print_usage(void) {
  printf("Usage: %s [options]\n\n", LINKSTAY_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: %s)\n",
         LINKSTAY_DEFAULT_TARGET);
  printf("  -i, --interval <sec>        Ping interval in seconds (default: "
         "%d)\n",
         LINKSTAY_DEFAULT_INTERVAL_SEC);
  printf("  -n, --threshold <num>       Consecutive failures threshold "
         "(default: %d)\n",
         LINKSTAY_DEFAULT_FAIL_THRESHOLD);
  printf("      --fail-threshold <num>  Clear alias for --threshold\n");
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: "
         "%d)\n\n",
         LINKSTAY_DEFAULT_TIMEOUT_MS);
  printf("Shutdown Options:\n");
  printf("  -m, --mode <mode>           Shutdown mode: %s\n",
         LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES);
  printf("                              (default: dry-run)\n");
  printf("                              true-off requires a systemd host with "
         "%s\n\n",
         LINKSTAY_SYSTEMCTL_PATH);
  printf("Logging Options:\n");
  printf("  -l, --log-level <level>     Log level: %s\n",
         LINKSTAY_CONFIG_LOG_LEVEL_VALUES);
  printf("                              (default: info; aliases: none=silent, "
         "warning=warn)\n");
  printf("System Integration:\n");
  printf("  -s[ARG], --systemd[=ARG]    Enable/disable systemd integration "
         "(default: %s)\n",
         LINKSTAY_DEFAULT_SYSTEMD ? "true" : "false");
  printf("                              Flag alone enables it; use "
         "--systemd=false, --systemd=0,\n");
  printf("                              -sfalse, or -s0 to disable\n");
  printf("                              Watchdog is auto-enabled with "
         "systemd\n");
  printf("                              Log timestamps are auto-disabled when "
         "systemd is enabled\n");
  printf("                              Otherwise timestamps stay enabled\n");
  printf("                              Accepted values: %s\n\n",
         LINKSTAY_CONFIG_BOOL_VALUES);
  printf("General Options:\n");
  printf("  -v, --version               Show version information\n");
  printf("  -h, --help                  Show this help message\n\n");
  printf("Environment Variables (lower priority than CLI args):\n");
  printf("  Network:      LINKSTAY_TARGET, LINKSTAY_INTERVAL,\n");
  printf("                LINKSTAY_THRESHOLD (alias: %s), LINKSTAY_TIMEOUT\n",
         LINKSTAY_THRESHOLD_ENV_ALIAS);
  printf("  Shutdown:     LINKSTAY_MODE\n");
  printf("  Logging:      LINKSTAY_LOG_LEVEL\n");
  printf("  Integration:  LINKSTAY_SYSTEMD\n");
  printf("\nExamples:\n");
  printf("  %s -t 1.1.1.1 -i 10 -n 5\n", LINKSTAY_PROGRAM_NAME);
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --mode true-off\n",
         LINKSTAY_PROGRAM_NAME);
  printf("  %s -t 8.8.8.8 -l debug --systemd=0\n", LINKSTAY_PROGRAM_NAME);
  printf("  %s -t 8.8.8.8 -i 5 -n 3 -m true-off -s0 -l debug\n",
         LINKSTAY_PROGRAM_NAME);
}

static void print_version(void) {
  printf("%s version %s\n", LINKSTAY_PROGRAM_NAME, LINKSTAY_VERSION);
  printf("LinkStay network monitor\n");
}

/* Pre-scan argv for --help/--version so we honor them even when env or other
 * arguments are invalid. Saves & restores getopt globals so the real parse
 * starts clean. */
static int scan_exit_option(int argc, char **argv) {
  int saved_optind = optind;
  int saved_opterr = opterr;
  int saved_optopt = optopt;
  char *saved_optarg = optarg;
  optind = 1;
  opterr = 0;

  int requested = 0;
  int option;
  while ((option = getopt_long(argc, argv, OPTSTRING, LONG_OPTIONS, NULL)) !=
         -1) {
    if (option == 'v' || option == 'h') {
      requested = option;
      break;
    }
  }

  optind = saved_optind;
  opterr = saved_opterr;
  optopt = saved_optopt;
  optarg = saved_optarg;
  return requested;
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

/* ---- Env & CLI loading ---- */

static bool resolve_threshold_env(const char **out_name,
                                  const char **out_value, char *error_msg,
                                  size_t error_size) {
  const char *primary = getenv(LINKSTAY_THRESHOLD_ENV);
  const char *alias = getenv(LINKSTAY_THRESHOLD_ENV_ALIAS);
  if (primary != NULL && alias != NULL && strcmp(primary, alias) != 0) {
    return config_errorf(error_msg, error_size,
                         "Conflicting values for %s and %s (use only one)",
                         LINKSTAY_THRESHOLD_ENV, LINKSTAY_THRESHOLD_ENV_ALIAS);
  }
  *out_name = primary != NULL ? LINKSTAY_THRESHOLD_ENV
                              : LINKSTAY_THRESHOLD_ENV_ALIAS;
  *out_value = primary != NULL ? primary : alias;
  return true;
}

static bool load_from_env(config_t *config, char *error_msg,
                          size_t error_size) {
  error_msg[0] = '\0';

  const char *target = getenv("LINKSTAY_TARGET");
  if (target != NULL &&
      !copy_string(config->target, sizeof(config->target), target,
                   "LINKSTAY_TARGET", error_msg, error_size)) {
    return false;
  }

  const char *threshold_name = NULL;
  const char *threshold_value = NULL;
  if (!resolve_threshold_env(&threshold_name, &threshold_value, error_msg,
                             error_size)) {
    return false;
  }
  if (threshold_value != NULL &&
      !parse_positive_int(threshold_name, threshold_value, "failures",
                          &config->fail_threshold, error_msg, error_size)) {
    return false;
  }

  const char *interval = getenv("LINKSTAY_INTERVAL");
  if (interval != NULL &&
      !parse_positive_int("LINKSTAY_INTERVAL", interval, "seconds",
                          &config->interval_sec, error_msg, error_size)) {
    return false;
  }

  const char *timeout = getenv("LINKSTAY_TIMEOUT");
  if (timeout != NULL &&
      !parse_positive_int("LINKSTAY_TIMEOUT", timeout, "milliseconds",
                          &config->timeout_ms, error_msg, error_size)) {
    return false;
  }

  const char *systemd_value = getenv("LINKSTAY_SYSTEMD");
  if (systemd_value != NULL &&
      !parse_bool(systemd_value, false, &config->enable_systemd)) {
    return config_invalid_value("LINKSTAY_SYSTEMD", systemd_value,
                                LINKSTAY_CONFIG_BOOL_VALUES, error_msg,
                                error_size);
  }

  const char *mode = getenv("LINKSTAY_MODE");
  if (mode != NULL && !parse_shutdown_mode(mode, &config->shutdown_mode)) {
    return config_invalid_value("LINKSTAY_MODE", mode,
                                LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES,
                                error_msg, error_size);
  }

  const char *log_level = getenv("LINKSTAY_LOG_LEVEL");
  if (log_level != NULL &&
      !parse_log_level(log_level, &config->log_level)) {
    return config_invalid_value("LINKSTAY_LOG_LEVEL", log_level,
                                LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES,
                                error_msg, error_size);
  }

  return true;
}

static bool load_from_cmdline(config_t *config, int argc, char **argv,
                              bool *exit_requested, char *error_msg,
                              size_t error_size) {
  *exit_requested = false;
  error_msg[0] = '\0';
  optind = 1;
  opterr = 0;

  int requested_exit = 0;
  int option;
  while ((option = getopt_long(argc, argv, OPTSTRING, LONG_OPTIONS, NULL)) !=
         -1) {
    switch (option) {
    case 't':
      if (!copy_string(config->target, sizeof(config->target), optarg,
                       "--target", error_msg, error_size)) {
        return false;
      }
      break;
    case 'i':
      if (!parse_positive_int("--interval", optarg, "seconds",
                              &config->interval_sec, error_msg, error_size)) {
        return false;
      }
      break;
    case 'n':
      if (!parse_positive_int("--threshold/--fail-threshold", optarg,
                              "failures", &config->fail_threshold, error_msg,
                              error_size)) {
        return false;
      }
      break;
    case 'w':
      if (!parse_positive_int("--timeout", optarg, "milliseconds",
                              &config->timeout_ms, error_msg, error_size)) {
        return false;
      }
      break;
    case 'm':
      if (!parse_shutdown_mode(optarg, &config->shutdown_mode)) {
        return config_invalid_value("--mode", optarg,
                                    LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES,
                                    error_msg, error_size);
      }
      break;
    case 'l':
      if (!parse_log_level(optarg, &config->log_level)) {
        return config_invalid_value("--log-level", optarg,
                                    LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES,
                                    error_msg, error_size);
      }
      break;
    case 's':
      if (!parse_bool(optarg, true, &config->enable_systemd)) {
        return config_invalid_value("--systemd", optarg,
                                    LINKSTAY_CONFIG_BOOL_VALUES, error_msg,
                                    error_size);
      }
      break;
    case 'v':
    case 'h':
      requested_exit = option;
      break;
    case '?':
      if (optopt != 0) {
        return config_errorf(
            error_msg, error_size,
            "Option requires an argument or has invalid value: -%c", optopt);
      }
      return config_errorf(error_msg, error_size, "Unknown option: %s",
                           argv[optind - 1] != NULL ? argv[optind - 1]
                                                    : "<unknown>");
    default:
      return config_errorf(error_msg, error_size,
                           "Failed to parse command line arguments");
    }
  }

  if (optind < argc) {
    return config_errorf(error_msg, error_size, "Unexpected argument: %s",
                         argv[optind]);
  }

  (void)handle_exit_option(requested_exit, exit_requested);
  return true;
}

bool config_resolve(config_t *restrict config, int argc, char **restrict argv,
                    bool *restrict exit_requested, char *restrict error_msg,
                    size_t error_size) {
  if (config == NULL || argv == NULL || exit_requested == NULL ||
      error_msg == NULL || error_size == 0) {
    return false;
  }

  *exit_requested = false;
  config_init_default(config);

  if (handle_exit_option(scan_exit_option(argc, argv), exit_requested)) {
    return true;
  }

  if (!load_from_env(config, error_msg, error_size) ||
      !load_from_cmdline(config, argc, argv, exit_requested, error_msg,
                         error_size)) {
    return false;
  }

  return *exit_requested || config_validate(config, error_msg, error_size);
}
