#ifndef LINKSTAY_H
#define LINKSTAY_H

#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include <stdbool.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#define LINKSTAY_VERSION "1.0"
#define LINKSTAY_PROGRAM_NAME "LinkStay"
#define LINKSTAY_MS_PER_SEC UINT64_C(1000)
#define LINKSTAY_MS_PER_MINUTE (UINT64_C(60) * LINKSTAY_MS_PER_SEC)
#define LINKSTAY_SYSTEMD_MESSAGE_SIZE 256U
#define LINKSTAY_SYSTEMD_STATUS_SIZE 240U
#define LINKSTAY_STATUS_DEDUP_WINDOW_MS UINT64_C(2000)
#define LINKSTAY_LOG_BUFFER_SIZE 2048U
#define LINKSTAY_EXIT_SUCCESS 0
#define LINKSTAY_EXIT_FAILURE 1
#define LINKSTAY_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Branch-prediction hints */
#define LINKSTAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LINKSTAY_LIKELY(x)   __builtin_expect(!!(x), 1)
#define LINKSTAY_COLD        __attribute__((cold))

typedef enum {
  LOG_LEVEL_SILENT = -1, /* completely silent, suitable for systemd */
  LOG_LEVEL_ERROR = 0,
  LOG_LEVEL_WARN = 1,
  LOG_LEVEL_INFO = 2, /* default */
  LOG_LEVEL_DEBUG = 3 /* verbose: prints per-ping latency */
} log_level_t;

typedef struct {
  log_level_t level;
  bool enable_timestamp;
} logger_t;

typedef struct {
  uint64_t total_pings;
  uint64_t successful_pings;
  uint64_t failed_pings;
  double total_latency;
  double min_latency; /* -1.0 sentinel: not yet recorded */
  double max_latency; /* -1.0 sentinel: not yet recorded */
  uint64_t start_time_ms;
} metrics_t;

typedef enum {
  SHUTDOWN_MODE_DRY_RUN,
  SHUTDOWN_MODE_TRUE_OFF,
  SHUTDOWN_MODE_LOG_ONLY
} shutdown_mode_t;

/* Target buffer: IPv6 max literal is 45 chars; 64 is ample. */
typedef struct {
  /* Network */
  char target[64];
  int interval_sec;
  int fail_threshold;
  int timeout_ms;

  /* Shutdown */
  shutdown_mode_t shutdown_mode;
  int delay_minutes;

  /* Logging */
  log_level_t log_level;

  /* Integration */
  bool enable_systemd;
} config_t;

typedef struct {
  bool success;
  double latency_ms;
  char error_msg[256];
} ping_result_t;

typedef enum {
  ICMP_RECEIVE_NO_MORE = -1,
  ICMP_RECEIVE_IGNORED = 0,
  ICMP_RECEIVE_MATCHED = 1,
  ICMP_RECEIVE_ERROR = 2,
} icmp_receive_status_t;

typedef enum {
  SHUTDOWN_RESULT_NO_ACTION = 0,
  SHUTDOWN_RESULT_TRIGGERED = 1,
  SHUTDOWN_RESULT_FAILED = 2,
} shutdown_result_t;

typedef struct {
  int sockfd;
  int family;
  uint16_t sequence;

  /* Send buffer (stack-allocated, zero-alloc model).
   * Aligned to 4 so casts to struct icmphdr / icmp6_hdr are well-defined. */
  alignas(4) uint8_t send_buf[256];
} icmp_pinger_t;

typedef struct {
  bool enabled;
  int sockfd;
  uint64_t watchdog_usec;
  uint64_t last_status_ms;
  char last_status[LINKSTAY_SYSTEMD_STATUS_SIZE];
} systemd_notifier_t;

typedef struct {
  void *backend_ctx;
  bool enabled;
  bool (*ready)(void *backend_ctx);
  bool (*status)(void *backend_ctx, const char *status);
  bool (*stopping)(void *backend_ctx);
  bool (*watchdog)(void *backend_ctx);
  uint64_t (*watchdog_interval_ms)(const void *backend_ctx);
  void (*destroy)(void *backend_ctx);
} runtime_services_t;

typedef struct linkstay_context {
  volatile sig_atomic_t stop_flag;
  int consecutive_fails;

  uint16_t
      cached_pid; /* cached getpid() & 0xFFFF, avoids syscall in hot path */

  config_t config;
  struct sockaddr_storage dest_addr;
  socklen_t dest_addr_len;
  logger_t logger;
  metrics_t metrics;
  icmp_pinger_t pinger;
  systemd_notifier_t systemd;
  runtime_services_t services;
} linkstay_ctx_t;

static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");
static_assert(sizeof(struct icmphdr) >= 8, "icmphdr must be at least 8 bytes");
static_assert(sizeof(sig_atomic_t) >= sizeof(int),
              "sig_atomic_t must be at least int size");

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp);
void logger_write(log_level_t level, bool enable_timestamp,
                  const char *restrict fmt, ...)
    __attribute__((format(printf, 3, 4)));
void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap);
[[nodiscard]] bool config_resolve(config_t *restrict config, int argc,
                                  char **restrict argv,
                                  bool *restrict exit_requested,
                                  char *restrict error_msg, size_t error_size);
bool config_log_timestamps_enabled(const config_t *restrict config);
void config_print(const config_t *restrict config,
                  const logger_t *restrict logger);
[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
                                    char *restrict error_msg,
                                    size_t error_size);
void icmp_pinger_destroy(icmp_pinger_t *restrict pinger);
[[nodiscard]] bool icmp_pinger_send_echo(
    icmp_pinger_t *restrict pinger,
    const struct sockaddr_storage *restrict dest_addr, socklen_t dest_addr_len,
    uint16_t identifier, size_t packet_len, char *restrict error_msg,
    size_t error_size);
icmp_receive_status_t icmp_pinger_receive_reply(
    const icmp_pinger_t *restrict pinger,
    const struct sockaddr_storage *restrict dest_addr, uint16_t identifier,
    uint16_t expected_sequence, uint64_t send_time_ms, uint64_t now_ms,
    ping_result_t *restrict out_result);
[[nodiscard]] bool resolve_target(const char *restrict target,
                                  struct sockaddr_storage *restrict addr,
                                  socklen_t *restrict addr_len,
                                  char *restrict error_msg, size_t error_size);
shutdown_result_t shutdown_trigger(const config_t *config,
                                   const logger_t *logger);
void systemd_notifier_init(systemd_notifier_t *restrict notifier);
void systemd_notifier_destroy(systemd_notifier_t *restrict notifier);
bool systemd_notifier_is_enabled(
    const systemd_notifier_t *restrict notifier);
bool systemd_notifier_ready(systemd_notifier_t *restrict notifier);
bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                             const char *restrict status);
bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier);
bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier);
uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t *restrict notifier);

/* logger_error / logger_warn are marked cold since they fire rarely.
   The level check avoids any formatting cost when the level is filtered.
   NULL logger is silently ignored so callers need not guard every call. */
#define DEFINE_LOGGER(name, lvl, attrs)                                        \
  static inline void attrs name(const logger_t *restrict logger,               \
                                const char *restrict fmt, ...)                 \
      __attribute__((format(printf, 2, 3)));                                   \
  static inline void attrs name(const logger_t *restrict logger,               \
                                const char *restrict fmt, ...) {               \
    if (LINKSTAY_LIKELY(logger != NULL) && logger->level >= (lvl)) {            \
      va_list args;                                                             \
      va_start(args, fmt);                                                      \
      logger_log_va(logger, (lvl), fmt, args);                                 \
      va_end(args);                                                             \
    }                                                                          \
  }

DEFINE_LOGGER(logger_error, LOG_LEVEL_ERROR, LINKSTAY_COLD)
DEFINE_LOGGER(logger_warn, LOG_LEVEL_WARN, LINKSTAY_COLD)
DEFINE_LOGGER(logger_info, LOG_LEVEL_INFO, )
DEFINE_LOGGER(logger_debug, LOG_LEVEL_DEBUG, )
const char *log_level_to_string(log_level_t level);
const char *shutdown_mode_to_string(shutdown_mode_t mode);
void log_shutdown_countdown(const logger_t *restrict logger,
                            shutdown_mode_t mode, int delay_minutes);
uint64_t get_monotonic_ms(void);

#endif // LINKSTAY_H
