#include "opts.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LS_DEFAULT_TARGET "1.1.1.1"
#define LS_DEFAULT_INTERVAL_SEC 10
#define LS_DEFAULT_FAIL_THRESHOLD 5
#define LS_DEFAULT_TIMEOUT_MS 2000
#define LS_DEFAULT_POWEROFF false
#define LS_DEFAULT_SYSTEMD true

#define LS_BOOL_VALUES "true|false|1|0|yes|no|on|off"
#define LS_LOG_LEVEL_VALUES "silent|error|warn|info|debug"
#define LS_SYSTEMCTL_PATH "/usr/bin/systemctl"
#define LS_SYSTEMD_RUNTIME_DIR "/run/systemd/system"

/* ---- Value parsers ---- */

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

/* ---- Option descriptor table ----
 *
 * One table drives every consumer: env loading, getopt optstring/longopts
 * construction, CLI parsing, and value application. Adding an option means
 * adding one row here plus (if user-visible) one line in print_usage(). */

typedef enum {
  OPT_STR,       /* bounded string copy into opts + offset */
  OPT_POS_INT,   /* int in 1..INT_MAX */
  OPT_BOOL,      /* named bool; bare CLI flag means true */
  OPT_LOG_LEVEL, /* ls_log_level_t by name */
  OPT_EXIT,      /* --version / --help: no value, handled by pre-scan */
} opt_kind_t;

typedef struct {
  const char *name;     /* long option */
  const char *env;      /* environment variable, or nullptr */
  size_t offset;        /* field position inside ls_opts_t */
  size_t size;          /* OPT_STR only: destination buffer size */
  const char *unit;     /* OPT_POS_INT only: unit for error messages */
  opt_kind_t kind;
  char letter;          /* short option */
} opt_desc_t;

static const opt_desc_t OPTION_TABLE[] = {
    {.letter = 't', .name = "target", .env = "LINKSTAY_TARGET",
     .kind = OPT_STR, .offset = offsetof(ls_opts_t, target),
     .size = sizeof(((ls_opts_t *)nullptr)->target)},
    {.letter = 'i', .name = "interval", .env = "LINKSTAY_INTERVAL",
     .kind = OPT_POS_INT, .offset = offsetof(ls_opts_t, interval_sec),
     .unit = "seconds"},
    {.letter = 'n', .name = "threshold", .env = "LINKSTAY_THRESHOLD",
     .kind = OPT_POS_INT, .offset = offsetof(ls_opts_t, fail_threshold),
     .unit = "failures"},
    {.letter = 'w', .name = "timeout", .env = "LINKSTAY_TIMEOUT",
     .kind = OPT_POS_INT, .offset = offsetof(ls_opts_t, timeout_ms),
     .unit = "milliseconds"},
    {.letter = 'p', .name = "poweroff", .env = "LINKSTAY_POWEROFF",
     .kind = OPT_BOOL, .offset = offsetof(ls_opts_t, poweroff)},
    {.letter = 'l', .name = "log-level", .env = "LINKSTAY_LOG_LEVEL",
     .kind = OPT_LOG_LEVEL, .offset = offsetof(ls_opts_t, log_level)},
    {.letter = 's', .name = "systemd", .env = "LINKSTAY_SYSTEMD",
     .kind = OPT_BOOL, .offset = offsetof(ls_opts_t, systemd)},
    {.letter = 'v', .name = "version", .kind = OPT_EXIT},
    {.letter = 'h', .name = "help", .kind = OPT_EXIT},
};

#define OPTION_COUNT LS_ARRAY_LEN(OPTION_TABLE)

/* Bool options take an optional argument: a bare flag means true. */
static bool opt_has_optional_arg(const opt_desc_t *desc) {
  return desc->kind == OPT_BOOL;
}

static bool opt_has_required_arg(const opt_desc_t *desc) {
  return desc->kind == OPT_STR || desc->kind == OPT_POS_INT ||
         desc->kind == OPT_LOG_LEVEL;
}

/* Renders the table into getopt inputs. optstring needs at most
 * 3 chars per option plus the terminator. */
static void build_getopt_inputs(char *optstring, size_t optstring_size,
                                struct option *longopts, size_t longopts_len) {
  size_t pos = 0;
  size_t li = 0;
  for (size_t i = 0; i < OPTION_COUNT && pos + 4 < optstring_size &&
                     li + 1 < longopts_len;
       i++) {
    const opt_desc_t *desc = &OPTION_TABLE[i];
    optstring[pos++] = desc->letter;
    int has_arg = no_argument;
    if (opt_has_required_arg(desc)) {
      optstring[pos++] = ':';
      has_arg = required_argument;
    } else if (opt_has_optional_arg(desc)) {
      optstring[pos++] = ':';
      optstring[pos++] = ':';
      has_arg = optional_argument;
    }
    longopts[li++] = (struct option){desc->name, has_arg, nullptr,
                                     desc->letter};
  }
  optstring[pos] = '\0';
  longopts[li] = (struct option){};
}

static const opt_desc_t *find_by_letter(int letter) {
  for (size_t i = 0; i < OPTION_COUNT; i++) {
    if (OPTION_TABLE[i].letter == letter) {
      return &OPTION_TABLE[i];
    }
  }
  return nullptr;
}

/* ---- Value application ---- */

static bool invalid_value(const char *name, const char *value,
                          const char *allowed, ls_err_t *err) {
  return ls_err_set(err, "Invalid value for %s: %s (use %s)", name,
                    value != nullptr ? value : "<empty>", allowed);
}

static bool apply_str(const opt_desc_t *desc, const char *name,
                      const char *value, ls_opts_t *opts, ls_err_t *err) {
  char *dest = (char *)opts + desc->offset;
  int written = snprintf(dest, desc->size, "%s", value);
  if (written < 0 || (size_t)written >= desc->size) {
    return ls_err_set(err, "%s is too long (max %zu characters)", name,
                      desc->size - 1);
  }
  return true;
}

static bool apply_pos_int(const opt_desc_t *desc, const char *name,
                          const char *value, ls_opts_t *opts, ls_err_t *err) {
  errno = 0;
  char *endptr = nullptr;
  long long parsed = strtoll(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0' || parsed < 1 ||
      parsed > INT_MAX) {
    return ls_err_set(err, "Invalid value for %s: %s (range 1..%d %s)", name,
                      value, INT_MAX, desc->unit);
  }
  *(int *)((char *)opts + desc->offset) = (int)parsed;
  return true;
}

static bool apply_bool(const opt_desc_t *desc, const char *name,
                       const char *value, bool bare_means_true,
                       ls_opts_t *opts, ls_err_t *err) {
  bool *dest = (bool *)((char *)opts + desc->offset);
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

static bool apply_log_level(const opt_desc_t *desc, const char *name,
                            const char *value, ls_opts_t *opts,
                            ls_err_t *err) {
  int parsed = 0;
  if (!lookup_named(LOG_LEVEL_NAMES, LS_ARRAY_LEN(LOG_LEVEL_NAMES), value,
                    &parsed)) {
    return invalid_value(name, value, LS_LOG_LEVEL_VALUES, err);
  }
  *(ls_log_level_t *)((char *)opts + desc->offset) = (ls_log_level_t)parsed;
  return true;
}

/* Dispatches one option value. `name` is the label used in error messages
 * (env var name or --long-option); `bare_means_true` applies to bool options
 * given as a bare CLI flag. */
static bool opt_apply(const opt_desc_t *desc, const char *name,
                      const char *value, bool bare_means_true, ls_opts_t *opts,
                      ls_err_t *err) {
  switch (desc->kind) {
  case OPT_STR:
    return apply_str(desc, name, value, opts, err);
  case OPT_POS_INT:
    return apply_pos_int(desc, name, value, opts, err);
  case OPT_BOOL:
    return apply_bool(desc, name, value, bare_means_true, opts, err);
  case OPT_LOG_LEVEL:
    return apply_log_level(desc, name, value, opts, err);
  case OPT_EXIT:
    return true;
  }
  return ls_err_set(err, "Internal option table corruption");
}

/* ---- Layers: defaults, environment, CLI ---- */

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

static bool load_env(ls_opts_t *opts, ls_err_t *err) {
  for (size_t i = 0; i < OPTION_COUNT; i++) {
    const opt_desc_t *desc = &OPTION_TABLE[i];
    if (desc->env == nullptr) {
      continue;
    }
    const char *value = getenv(desc->env);
    if (value == nullptr) {
      continue;
    }
    if (!opt_apply(desc, desc->env, value, false, opts, err)) {
      return false;
    }
  }
  return true;
}

static bool load_cli(ls_opts_t *opts, int argc, char **argv,
                     int *requested_exit, ls_err_t *err) {
  char optstring[OPTION_COUNT * 3 + 1];
  struct option longopts[OPTION_COUNT + 1];
  build_getopt_inputs(optstring, sizeof(optstring), longopts,
                      LS_ARRAY_LEN(longopts));

  *requested_exit = 0;
  optind = 1;
  opterr = 0;

  int letter;
  while ((letter = getopt_long(argc, argv, optstring, longopts, nullptr)) !=
         -1) {
    if (letter == '?') {
      if (optopt != 0) {
        return ls_err_set(
            err, "Option requires an argument or has invalid value: -%c",
            optopt);
      }
      return ls_err_set(err, "Unknown option: %s",
                        argv[optind - 1] != nullptr ? argv[optind - 1]
                                                    : "<unknown>");
    }

    const opt_desc_t *desc = find_by_letter(letter);
    if (desc == nullptr) {
      return ls_err_set(err, "Failed to parse command line arguments");
    }
    if (desc->kind == OPT_EXIT) {
      *requested_exit = letter;
      continue;
    }

    char name[64];
    (void)snprintf(name, sizeof(name), "--%s", desc->name);
    if (!opt_apply(desc, name, optarg, true, opts, err)) {
      return false;
    }
  }

  if (optind < argc) {
    return ls_err_set(err, "Unexpected argument: %s", argv[optind]);
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
  struct stat systemctl_stat;
  struct stat runtime_dir_stat;
  /* Defense-in-depth beyond the executability check: require systemctl to be
   * owned by root and not world-writable, so a compromised/replaced binary
   * at this well-known path cannot be silently trusted when poweroff is on. */
  return access(LS_SYSTEMCTL_PATH, X_OK) == 0 &&
         stat(LS_SYSTEMCTL_PATH, &systemctl_stat) == 0 &&
         systemctl_stat.st_uid == 0 && !(systemctl_stat.st_mode & S_IWOTH) &&
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

  uint64_t interval_ms = 0;
  if (ckd_mul(&interval_ms, (uint64_t)opts->interval_sec, LS_MS_PER_SEC)) {
    return ls_err_set(
        err, "Interval is too large to convert safely to milliseconds");
  }
  if ((uint64_t)opts->timeout_ms >= interval_ms) {
    return ls_err_set(err,
                      "Timeout (%d ms) must be smaller than interval (%d s = "
                      "%" PRIu64 " ms) to avoid overlapping probes",
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
  printf("  -w, --timeout <ms>          Ping timeout in milliseconds "
         "(default: %d)\n\n",
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
  printf("linkstay network monitor\n");
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

/* Pre-scans argv for --help/--version so they are honored even when the
 * environment or other arguments are invalid. Saves and restores getopt
 * globals so the real parse starts clean. */
static int scan_exit_option(int argc, char **argv) {
  char optstring[OPTION_COUNT * 3 + 1];
  struct option longopts[OPTION_COUNT + 1];
  build_getopt_inputs(optstring, sizeof(optstring), longopts,
                      LS_ARRAY_LEN(longopts));

  int saved_optind = optind;
  int saved_opterr = opterr;
  int saved_optopt = optopt;
  char *saved_optarg = optarg;
  optind = 1;
  opterr = 0;

  int requested = 0;
  int letter;
  while ((letter = getopt_long(argc, argv, optstring, longopts, nullptr)) !=
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

  int requested_exit = 0;
  if (!load_env(opts, err) ||
      !load_cli(opts, argc, argv, &requested_exit, err)) {
    return false;
  }

  if (handle_exit_option(requested_exit, exit_requested)) {
    return true;
  }
  return validate(opts, err);
}
