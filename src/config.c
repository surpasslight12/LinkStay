#include "linkstay.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
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
#define LINKSTAY_DEFAULT_DELAY_MINUTES 0
#define LINKSTAY_MAX_DELAY_MINUTES (365 * 24 * 60)
#define LINKSTAY_DEFAULT_SYSTEMD true
#define LINKSTAY_FAIL_THRESHOLD_ENV_ALIAS "LINKSTAY_FAIL_THRESHOLD"

#define LINKSTAY_CONFIG_BOOL_VALUES "true|false|1|0|yes|no|on|off"
#define LINKSTAY_CONFIG_LOG_LEVEL_VALUES "silent|error|warn|info|debug"
#define LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES                                 \
  "silent|error|warn|info|debug (aliases: none=silent, warning=warn)"
#define LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES "dry-run|true-off|log-only"
#define LINKSTAY_SYSTEMCTL_PATH "/usr/bin/systemctl"
#define LINKSTAY_SYSTEMD_RUNTIME_DIR "/run/systemd/system"

typedef struct {
  const char *name;
  int value;
} config_named_value_t;

typedef struct {
  int option_value;
  const char *name;
  int min_value;
  int max_value;
  const char *unit_name;
  int *destination;
} config_int_binding_t;

static const struct option CONFIG_LONG_OPTIONS[] = {
    {"target", required_argument, 0, 't'},
    {"interval", required_argument, 0, 'i'},
    {"threshold", required_argument, 0, 'n'},
    {"fail-threshold", required_argument, 0, 'n'},
    {"timeout", required_argument, 0, 'w'},
    {"mode", required_argument, 0, 'm'},
    {"delay", required_argument, 0, 'd'},
    {"log-level", required_argument, 0, 'l'},
    {"systemd", optional_argument, 0, 's'},
    {"version", no_argument, 0, 'v'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0},
};

static const char *const CONFIG_OPTSTRING = "t:i:n:w:m:d:l:s::vh";

static const config_named_value_t CONFIG_BOOL_OPTIONS[] = {
    {"true", 1},  {"1", 1},   {"yes", 1}, {"on", 1},
    {"false", 0}, {"0", 0},   {"no", 0},  {"off", 0},
};

static const config_named_value_t CONFIG_LOG_LEVEL_OPTIONS[] = {
    {"silent", LOG_LEVEL_SILENT}, {"none", LOG_LEVEL_SILENT},
    {"error", LOG_LEVEL_ERROR},   {"warn", LOG_LEVEL_WARN},
    {"warning", LOG_LEVEL_WARN},  {"info", LOG_LEVEL_INFO},
    {"debug", LOG_LEVEL_DEBUG},
};

static const config_named_value_t CONFIG_SHUTDOWN_MODE_OPTIONS[] = {
    {"dry-run", SHUTDOWN_MODE_DRY_RUN},
    {"true-off", SHUTDOWN_MODE_TRUE_OFF},
    {"log-only", SHUTDOWN_MODE_LOG_ONLY},
};

static bool config_parse_named_value(const config_named_value_t *options,
                                     size_t option_count,
                                     const char *restrict arg,
                                     int *restrict out_value);

static bool config_errorf(char *restrict error_msg, size_t error_size,
                          const char *restrict fmt, ...) {
  if (error_msg == NULL || error_size == 0 || fmt == NULL) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  vsnprintf(error_msg, error_size, fmt, args);
  va_end(args);
  return false;
}

static bool config_string_equals_ignore_case(const char *restrict lhs,
                                             const char *restrict rhs) {
  return lhs != NULL && rhs != NULL && strcasecmp(lhs, rhs) == 0;
}

static bool config_copy_string_value(char *restrict dest, size_t dest_size,
                                     const char *restrict src,
                                     const char *restrict name,
                                     char *restrict error_msg,
                                     size_t error_size) {
  if (dest == NULL || dest_size == 0 || src == NULL || name == NULL) {
    return false;
  }

  int written = snprintf(dest, dest_size, "%s", src);
  if (written < 0 || (size_t)written >= dest_size) {
    return config_errorf(error_msg, error_size,
                         "%s is too long (max %zu characters)", name,
                         dest_size - 1);
  }

  return true;
}

static bool config_parse_bool_value(const char *restrict arg,
                                    bool implicit_true_when_null,
                                    bool *restrict out_value) {
  if (out_value == NULL) {
    return false;
  }

  if (arg == NULL) {
    if (!implicit_true_when_null) {
      return false;
    }

    *out_value = true;
    return true;
  }

  int parsed = 0;
  if (!config_parse_named_value(CONFIG_BOOL_OPTIONS,
                                LINKSTAY_ARRAY_LEN(CONFIG_BOOL_OPTIONS), arg,
                                &parsed)) {
    return false;
  }

  *out_value = parsed != 0;
  return true;
}

static bool config_parse_int_value(const char *restrict arg, int min_value,
                                   int max_value, int *restrict out_value) {
  if (arg == NULL || out_value == NULL) {
    return false;
  }

  errno = 0;
  char *endptr = NULL;
  long long value = strtoll(arg, &endptr, 10);
  if (errno != 0 || endptr == arg || *endptr != '\0') {
    return false;
  }

  if (value < (long long)min_value || value > (long long)max_value ||
      value < INT_MIN || value > INT_MAX) {
    return false;
  }

  *out_value = (int)value;
  return true;
}

static bool config_parse_named_value(const config_named_value_t *options,
                                     size_t option_count,
                                     const char *restrict arg,
                                     int *restrict out_value) {
  if (option_count == 0 || arg == NULL || out_value == NULL) {
    return false;
  }

  for (size_t i = 0; i < option_count; i++) {
    if (config_string_equals_ignore_case(arg, options[i].name)) {
      *out_value = options[i].value;
      return true;
    }
  }

  return false;
}

static const char *
config_named_value_to_string(const config_named_value_t *options,
                             size_t option_count, int value,
                             const char *fallback) {
  if (option_count == 0) {
    return fallback;
  }

  for (size_t i = 0; i < option_count; i++) {
    if (options[i].value == value) {
      return options[i].name;
    }
  }

  return fallback;
}

static bool config_parse_log_level(const char *restrict arg,
                                   log_level_t *restrict out_value) {
  int parsed = 0;
  if (!config_parse_named_value(CONFIG_LOG_LEVEL_OPTIONS,
                                LINKSTAY_ARRAY_LEN(CONFIG_LOG_LEVEL_OPTIONS),
                                arg, &parsed)) {
    return false;
  }

  *out_value = (log_level_t)parsed;
  return true;
}

static bool config_parse_shutdown_mode(const char *restrict arg,
                                       shutdown_mode_t *restrict out_value) {
  int parsed = 0;
  if (!config_parse_named_value(
          CONFIG_SHUTDOWN_MODE_OPTIONS,
          LINKSTAY_ARRAY_LEN(CONFIG_SHUTDOWN_MODE_OPTIONS), arg, &parsed)) {
    return false;
  }

  *out_value = (shutdown_mode_t)parsed;
  return true;
}

static const char *config_optarg_or_empty(const char *restrict arg) {
  return arg != NULL ? arg : "<empty>";
}

static bool config_resolve_env_value(const char *restrict primary_name,
                                     const char *restrict alias_name,
                                     const char **restrict out_name,
                                     const char **restrict out_value,
                                     char *restrict error_msg,
                                     size_t error_size) {
  if (primary_name == NULL || alias_name == NULL || out_name == NULL ||
      out_value == NULL) {
    return false;
  }

  const char *primary_value = getenv(primary_name);
  const char *alias_value = getenv(alias_name);
  if (primary_value != NULL && alias_value != NULL &&
      strcmp(primary_value, alias_value) != 0) {
    return config_errorf(error_msg, error_size,
                         "Conflicting values for %s and %s (use only one)",
                         primary_name, alias_name);
  }

  if (primary_value != NULL) {
    *out_name = primary_name;
    *out_value = primary_value;
    return true;
  }

  if (alias_value != NULL) {
    *out_name = alias_name;
    *out_value = alias_value;
    return true;
  }

  *out_name = primary_name;
  *out_value = NULL;
  return true;
}

static bool config_error_invalid_value(const char *restrict name,
                                       const char *restrict value,
                                       const char *restrict allowed_values,
                                       char *restrict error_msg,
                                       size_t error_size) {
  return config_errorf(error_msg, error_size,
                       "Invalid value for %s: %s (use %s)", name,
                       config_optarg_or_empty(value), allowed_values);
}

static bool config_parse_bound_int(const config_int_binding_t *restrict binding,
                                   const char *restrict value,
                                   char *restrict error_msg,
                                   size_t error_size) {
  int parsed_value = 0;
  if (binding == NULL || binding->name == NULL ||
      binding->destination == NULL) {
    return false;
  }

  if (!config_parse_int_value(value, binding->min_value, binding->max_value,
                              &parsed_value)) {
    return config_errorf(
        error_msg, error_size, "Invalid value for %s: %s (range %d..%d %s)",
        binding->name, config_optarg_or_empty(value), binding->min_value,
        binding->max_value, binding->unit_name);
  }

  *binding->destination = parsed_value;
  return true;
}

static const config_int_binding_t *
config_find_int_binding(const config_int_binding_t *bindings,
                        size_t binding_count, int option_value) {
  for (size_t i = 0; i < binding_count; i++) {
    if (bindings[i].option_value == option_value) {
      return &bindings[i];
    }
  }

  return NULL;
}

static void config_init_default(config_t *restrict config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  (void)snprintf(config->target, sizeof(config->target), "%s",
                 LINKSTAY_DEFAULT_TARGET);
  config->interval_sec = LINKSTAY_DEFAULT_INTERVAL_SEC;
  config->fail_threshold = LINKSTAY_DEFAULT_FAIL_THRESHOLD;
  config->timeout_ms = LINKSTAY_DEFAULT_TIMEOUT_MS;
  config->shutdown_mode = SHUTDOWN_MODE_DRY_RUN;
  config->delay_minutes = LINKSTAY_DEFAULT_DELAY_MINUTES;
  config->log_level = LOG_LEVEL_INFO;
  config->enable_systemd = LINKSTAY_DEFAULT_SYSTEMD;
}

static bool config_is_valid_ip_literal(const char *restrict target) {
  if (target == NULL || target[0] == '\0') {
    return false;
  }

  unsigned char addr_buffer[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, target, addr_buffer) == 1 ||
         inet_pton(AF_INET6, target, addr_buffer) == 1;
}

static bool config_timeout_fits_interval(const config_t *restrict config) {
  if (config == NULL || config->interval_sec <= 0 || config->timeout_ms <= 0) {
    return false;
  }

  uint64_t interval_ms = 0;
  if (ckd_mul(&interval_ms, (uint64_t)config->interval_sec,
              LINKSTAY_MS_PER_SEC)) {
    return false;
  }

  return (uint64_t)config->timeout_ms < interval_ms;
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
  return config_named_value_to_string(
      CONFIG_SHUTDOWN_MODE_OPTIONS,
      LINKSTAY_ARRAY_LEN(CONFIG_SHUTDOWN_MODE_OPTIONS), mode, "unknown");
}

static bool config_validate(const config_t *restrict config,
                            char *restrict error_msg, size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  if (config->target[0] == '\0') {
    return config_errorf(error_msg, error_size, "Target host cannot be empty");
  }

  if (!config_is_valid_ip_literal(config->target)) {
    return config_errorf(
        error_msg, error_size,
        "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }

  if (config->interval_sec <= 0) {
    return config_errorf(error_msg, error_size, "Interval must be positive");
  }

  if (config->fail_threshold <= 0) {
    return config_errorf(error_msg, error_size,
                         "Failure threshold must be positive");
  }

  if (config->timeout_ms <= 0) {
    return config_errorf(error_msg, error_size, "Timeout must be positive");
  }

  if (config->delay_minutes < 0) {
    return config_errorf(error_msg, error_size,
                         "Delay minutes cannot be negative");
  }

  if (config->delay_minutes > LINKSTAY_MAX_DELAY_MINUTES) {
    return config_errorf(error_msg, error_size,
                         "Delay minutes too large (max 525600)");
  }

  if (!config_timeout_fits_interval(config)) {
    uint64_t interval_ms = 0;
    if (ckd_mul(&interval_ms, (uint64_t)config->interval_sec,
                LINKSTAY_MS_PER_SEC)) {
      return config_errorf(
          error_msg, error_size,
          "Interval is too large to convert safely to milliseconds");
    }

    return config_errorf(
        error_msg, error_size,
        "Timeout (%d ms) must be smaller than interval (%d s = %" PRIu64
        " ms) to avoid overlapping probes",
        config->timeout_ms, config->interval_sec, interval_ms);
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY &&
      config->delay_minutes != 0) {
    return config_errorf(
        error_msg, error_size,
        "Delay is only valid with dry-run or true-off shutdown modes");
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_TRUE_OFF) {
    struct stat runtime_dir_stat;
    bool systemctl_available = access(LINKSTAY_SYSTEMCTL_PATH, X_OK) == 0;
    bool systemd_runtime_available =
        stat(LINKSTAY_SYSTEMD_RUNTIME_DIR, &runtime_dir_stat) == 0 &&
        S_ISDIR(runtime_dir_stat.st_mode);
    if (!systemctl_available || !systemd_runtime_available) {
      return config_errorf(
          error_msg, error_size,
          "true-off requires a systemd host with %s and %s available",
          LINKSTAY_SYSTEMCTL_PATH, LINKSTAY_SYSTEMD_RUNTIME_DIR);
    }
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
  logger_debug(logger, "  Delay: %d minutes", config->delay_minutes);
  logger_debug(logger, "  Log Level: %s",
               log_level_to_string(config->log_level));
  logger_debug(logger, "  Timestamp: %s",
               config_log_timestamps_enabled(config) ? "true" : "false");
  logger_debug(logger, "  Systemd: %s",
               config->enable_systemd ? "true" : "false");
}

static void config_print_usage(void) {
  printf("Usage: %s [options]\n\n", LINKSTAY_PROGRAM_NAME);
  printf("Network Options:\n");
  printf("  -t, --target <ip>           Target IP literal to monitor (DNS "
         "disabled, default: %s)\n",
         LINKSTAY_DEFAULT_TARGET);
  printf(
      "  -i, --interval <sec>        Ping interval in seconds (default: %d)\n",
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
  printf("  -d, --delay <min>           Shutdown countdown in minutes for "
         "dry-run/true-off mode\n");
  printf("                              (required numeric argument, range: "
         "0..%d, default: %d)\n",
         LINKSTAY_MAX_DELAY_MINUTES, LINKSTAY_DEFAULT_DELAY_MINUTES);
  printf("                              0 means immediate execution without "
         "countdown\n\n");
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
  printf(
      "                              Watchdog is auto-enabled with systemd\n");
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
         LINKSTAY_FAIL_THRESHOLD_ENV_ALIAS);
  printf("  Shutdown:     LINKSTAY_MODE, LINKSTAY_DELAY\n");
  printf("  Logging:      LINKSTAY_LOG_LEVEL\n");
  printf("  Integration:  LINKSTAY_SYSTEMD\n");
  printf("\n");
  printf("Examples:\n");
  printf("  # Basic monitoring with dry-run mode\n");
  printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", LINKSTAY_PROGRAM_NAME);
  printf("  # Production mode (actual shutdown)\n");
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --mode true-off\n\n",
         LINKSTAY_PROGRAM_NAME);
  printf("  # true-off requires a systemd host with /run/systemd/system\n");
  printf("  # Delayed countdown before shutdown\n");
  printf("  %s -t 192.168.1.1 -i 5 -n 3 --mode true-off --delay 3\n\n",
         LINKSTAY_PROGRAM_NAME);
  printf("  # Foreground debug mode with local timestamps\n");
  printf("  %s -t 8.8.8.8 -l debug --systemd=0\n\n", LINKSTAY_PROGRAM_NAME);
  printf("  # Short options (required args may be attached or separated; "
         "optional -s "
         "arg should be attached)\n");
  printf("  %s -t 8.8.8.8 -i 5 -n 3 -m true-off -d 0 -s0 -l debug\n\n",
         LINKSTAY_PROGRAM_NAME);
}

static void config_print_version(void) {
  printf("%s version %s\n", LINKSTAY_PROGRAM_NAME, LINKSTAY_VERSION);
  printf("LinkStay network monitor\n");
}

static bool config_load_from_env(config_t *restrict config,
                                 char *restrict error_msg, size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  error_msg[0] = '\0';

  const char *target = getenv("LINKSTAY_TARGET");
  if (target != NULL &&
      !config_copy_string_value(config->target, sizeof(config->target), target,
                                "LINKSTAY_TARGET", error_msg, error_size)) {
    return false;
  }

  const char *threshold_name = NULL;
  const char *threshold_value = NULL;
  if (!config_resolve_env_value("LINKSTAY_THRESHOLD",
                                LINKSTAY_FAIL_THRESHOLD_ENV_ALIAS,
                                &threshold_name, &threshold_value, error_msg,
                                error_size)) {
    return false;
  }
  if (threshold_value != NULL) {
    config_int_binding_t threshold_binding = {
        0, threshold_name, 1, INT_MAX, "failures", &config->fail_threshold};
    if (!config_parse_bound_int(&threshold_binding, threshold_value, error_msg,
                                error_size)) {
      return false;
    }
  }

  const config_int_binding_t int_bindings[] = {
      {0, "LINKSTAY_INTERVAL", 1, INT_MAX, "seconds", &config->interval_sec},
      {0, "LINKSTAY_TIMEOUT", 1, INT_MAX, "milliseconds", &config->timeout_ms},
      {0, "LINKSTAY_DELAY", 0, LINKSTAY_MAX_DELAY_MINUTES, "minutes",
       &config->delay_minutes},
  };
  for (size_t i = 0; i < LINKSTAY_ARRAY_LEN(int_bindings); i++) {
    const char *value = getenv(int_bindings[i].name);
    if (value != NULL && !config_parse_bound_int(&int_bindings[i], value,
                                                 error_msg, error_size)) {
      return false;
    }
  }

  const char *systemd_value = getenv("LINKSTAY_SYSTEMD");
  if (systemd_value != NULL &&
      !config_parse_bool_value(systemd_value, false, &config->enable_systemd)) {
    return config_error_invalid_value("LINKSTAY_SYSTEMD", systemd_value,
                                      LINKSTAY_CONFIG_BOOL_VALUES, error_msg,
                                      error_size);
  }

  const char *shutdown_mode = getenv("LINKSTAY_MODE");
  if (shutdown_mode != NULL &&
      !config_parse_shutdown_mode(shutdown_mode, &config->shutdown_mode)) {
    return config_error_invalid_value("LINKSTAY_MODE", shutdown_mode,
                                      LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES,
                                      error_msg, error_size);
  }

  const char *log_level = getenv("LINKSTAY_LOG_LEVEL");
  if (log_level != NULL &&
      !config_parse_log_level(log_level, &config->log_level)) {
    return config_error_invalid_value("LINKSTAY_LOG_LEVEL", log_level,
                                      LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES,
                                      error_msg, error_size);
  }

  return true;
}

static bool config_load_from_cmdline(config_t *restrict config, int argc,
                                     char **restrict argv,
                                     bool *restrict exit_requested,
                                     char *restrict error_msg,
                                     size_t error_size) {
  if (config == NULL || argv == NULL || exit_requested == NULL ||
      error_msg == NULL || error_size == 0) {
    return false;
  }

  const config_int_binding_t int_bindings[] = {
      {'i', "--interval", 1, INT_MAX, "seconds", &config->interval_sec},
      {'n', "--threshold/--fail-threshold", 1, INT_MAX, "failures",
       &config->fail_threshold},
      {'w', "--timeout", 1, INT_MAX, "milliseconds", &config->timeout_ms},
      {'d', "--delay", 0, LINKSTAY_MAX_DELAY_MINUTES, "minutes",
       &config->delay_minutes},
  };

  *exit_requested = false;
  error_msg[0] = '\0';
  optind = 1;
  opterr = 0;

  int requested_exit_option = 0;
  int option_index = 0;
  int option = 0;
  while ((option = getopt_long(argc, argv, CONFIG_OPTSTRING,
                               CONFIG_LONG_OPTIONS, &option_index)) != -1) {
    switch (option) {
    case 't':
      if (!config_copy_string_value(config->target, sizeof(config->target),
                                    optarg, "--target", error_msg,
                                    error_size)) {
        return false;
      }
      break;
    case 'i':
    case 'n':
    case 'w':
    case 'd': {
      const config_int_binding_t *binding = config_find_int_binding(
          int_bindings, LINKSTAY_ARRAY_LEN(int_bindings), option);
      if (binding == NULL) {
        return config_errorf(error_msg, error_size,
                             "Failed to map command-line integer option: -%c",
                             option);
      }
      if (!config_parse_bound_int(binding, optarg, error_msg, error_size)) {
        return false;
      }
      break;
    }
    case 'm':
      if (!config_parse_shutdown_mode(optarg, &config->shutdown_mode)) {
        return config_error_invalid_value("--mode", optarg,
                                          LINKSTAY_CONFIG_SHUTDOWN_MODE_VALUES,
                                          error_msg, error_size);
      }
      break;
    case 'l':
      if (!config_parse_log_level(optarg, &config->log_level)) {
        return config_error_invalid_value("--log-level", optarg,
                                          LINKSTAY_CONFIG_LOG_LEVEL_ALLOWED_VALUES,
                                          error_msg, error_size);
      }
      break;
    case 's':
      if (!config_parse_bool_value(optarg, true, &config->enable_systemd)) {
        return config_error_invalid_value("--systemd", optarg,
                                          LINKSTAY_CONFIG_BOOL_VALUES,
                                          error_msg, error_size);
      }
      break;
    case 'v':
      requested_exit_option = 'v';
      break;
    case 'h':
      requested_exit_option = 'h';
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

  if (requested_exit_option == 'v') {
    config_print_version();
    *exit_requested = true;
  } else if (requested_exit_option == 'h') {
    config_print_usage();
    *exit_requested = true;
  }

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
  if (!config_load_from_env(config, error_msg, error_size)) {
    return false;
  }

  if (!config_load_from_cmdline(config, argc, argv, exit_requested, error_msg,
                                error_size)) {
    return false;
  }

  return *exit_requested || config_validate(config, error_msg, error_size);
}
