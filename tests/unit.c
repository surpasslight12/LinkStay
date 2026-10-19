/*
 * unit.c — unit tests for the stateless helpers in base, addr, log, loop, and
 * stats, plus the opts resolver.
 *
 * Built and run by `make unit-test`. No framework, no allocation: a failing
 * check prints its source location and the process exits non-zero.
 */

#include "addr.h"
#include "base.h"
#include "log.h"
#include "loop.h"
#include "opts.h"
#include "stats.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (strcmp((actual), (expected)) != 0) {                                   \
      (void)fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,   \
                    (actual), (expected));                                     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void test_error_type(void) {
  ls_err_t err = {};
  CHECK(!ls_err_set(&err, "code %d", 7));
  CHECK_STR(err.msg, "code 7");
  CHECK(!ls_err_set(nullptr, "ignored"));

  char long_text[300];
  memset(long_text, 'x', sizeof(long_text));
  long_text[sizeof(long_text) - 1] = '\0';
  CHECK(!ls_err_set(&err, "%s", long_text));
  CHECK(strlen(err.msg) == sizeof(err.msg) - 1);
}

static void test_saturating_math(void) {
  CHECK(ls_add_sat(1, 2) == 3);
  CHECK(ls_add_sat(UINT64_MAX - 1, 1) == UINT64_MAX);
  CHECK(ls_add_sat(UINT64_MAX, 1) == UINT64_MAX);

  CHECK(ls_mul_sat(3, 4) == 12);
  CHECK(ls_mul_sat(UINT64_MAX, 0) == 0);
  CHECK(ls_mul_sat(UINT64_MAX, 2) == UINT64_MAX);
  CHECK(ls_mul_sat(UINT64_MAX / 2 + 1, 2) == UINT64_MAX);
}

static void test_duration_format(void) {
  char buf[64];
  ls_format_duration(0, buf, sizeof(buf));
  CHECK_STR(buf, "0ms");
  ls_format_duration(999, buf, sizeof(buf));
  CHECK_STR(buf, "999ms");
  ls_format_duration(1000, buf, sizeof(buf));
  CHECK_STR(buf, "1s");
  ls_format_duration(1500, buf, sizeof(buf));
  CHECK_STR(buf, "1500ms");
  ls_format_duration(60000, buf, sizeof(buf));
  CHECK_STR(buf, "60s");
}

static void test_latency_format(void) {
  char buf[64];
  ls_format_latency(-1.0, buf, sizeof(buf));
  CHECK_STR(buf, "0.000ms");
  ls_format_latency(0.004, buf, sizeof(buf));
  CHECK_STR(buf, "0.004ms");
  ls_format_latency(1.0, buf, sizeof(buf));
  CHECK_STR(buf, "1.00ms");
  ls_format_latency(42.5, buf, sizeof(buf));
  CHECK_STR(buf, "42.50ms");
  ls_format_latency(100.0, buf, sizeof(buf));
  CHECK_STR(buf, "100.0ms");
  ls_format_latency(150.5, buf, sizeof(buf));
  CHECK_STR(buf, "150.5ms");
}

static void test_uptime_format(void) {
  char buf[64];
  ls_format_uptime(0, buf, sizeof(buf));
  CHECK_STR(buf, "0s");
  ls_format_uptime(59, buf, sizeof(buf));
  CHECK_STR(buf, "59s");
  ls_format_uptime(60, buf, sizeof(buf));
  CHECK_STR(buf, "1m0s");
  ls_format_uptime(3599, buf, sizeof(buf));
  CHECK_STR(buf, "59m59s");
  ls_format_uptime(3600, buf, sizeof(buf));
  CHECK_STR(buf, "1h0m0s");
  ls_format_uptime(86400, buf, sizeof(buf));
  CHECK_STR(buf, "1d0h0m0s");
  ls_format_uptime(90061, buf, sizeof(buf));
  CHECK_STR(buf, "1d1h1m1s");
  ls_format_uptime(UINT64_MAX, buf, sizeof(buf));
  CHECK_STR(buf, "213503982334601d7h0m15s");

  char small[5];
  ls_format_uptime(3661, small, sizeof(small));
  CHECK_STR(small, "1h1m");
}

static void test_addr_parse(void) {
  struct sockaddr_storage addr = {};
  socklen_t len = 0;
  ls_err_t err = {};

  CHECK(ls_addr_parse("127.0.0.1", &addr, &len, &err));
  CHECK((int)addr.ss_family == AF_INET);
  CHECK(len == (socklen_t)sizeof(struct sockaddr_in));
  const struct sockaddr_in *addr4 = (const struct sockaddr_in *)&addr;
  CHECK(addr4->sin_addr.s_addr == htonl(INADDR_LOOPBACK));

  memset(&addr, 0, sizeof(addr));
  len = 0;
  err = (ls_err_t){};
  CHECK(ls_addr_parse("::1", &addr, &len, &err));
  CHECK((int)addr.ss_family == AF_INET6);
  CHECK(len == (socklen_t)sizeof(struct sockaddr_in6));
  const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)&addr;
  CHECK(IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr));

  CHECK(ls_addr_is_literal("1.1.1.1"));
  CHECK(ls_addr_is_literal("2606:4700:4700::1111"));
  CHECK(!ls_addr_is_literal("example.com"));
  CHECK(!ls_addr_is_literal(""));
  CHECK(!ls_addr_is_literal(nullptr));

  err = (ls_err_t){};
  CHECK(!ls_addr_parse("example.com", &addr, &len, &err));
  CHECK(strstr(err.msg, "DNS disabled") != nullptr);
  CHECK(strstr(err.msg, "IPv4 literal") != nullptr);

  err = (ls_err_t){};
  CHECK(!ls_addr_parse("::g", &addr, &len, &err));
  CHECK(strstr(err.msg, "IPv6 literal") != nullptr);

  err = (ls_err_t){};
  CHECK(!ls_addr_parse(nullptr, &addr, &len, &err));
  CHECK_STR(err.msg, "Invalid address parse arguments");
}

static void test_timer_helpers(void) {
  ls_timer_t timer = {.deadline_ms = UINT64_MAX};
  CHECK(!ls_timer_armed(&timer));

  ls_timer_arm_after(&timer, 1000, 500);
  CHECK(timer.deadline_ms == 1500);
  CHECK(ls_timer_armed(&timer));

  ls_timer_step(&timer, 500, 1400);
  CHECK(timer.deadline_ms == 2000);

  ls_timer_step(&timer, 500, 3000);
  CHECK(timer.deadline_ms == 3500);

  ls_timer_step(&timer, 0, 4000);
  CHECK(!ls_timer_armed(&timer));

  ls_timer_disarm(&timer);
  ls_timer_step(&timer, 100, 500);
  CHECK(timer.deadline_ms == 600);

  ls_timer_arm_after(&timer, UINT64_MAX - 2, 10);
  CHECK(timer.deadline_ms == UINT64_MAX);
}

static const char *const OPT_ENV_NAMES[] = {
    "LINKSTAY_TARGET",  "LINKSTAY_INTERVAL", "LINKSTAY_THRESHOLD",
    "LINKSTAY_TIMEOUT", "LINKSTAY_POWEROFF", "LINKSTAY_LOG_LEVEL",
    "LINKSTAY_SYSTEMD",
};

static void reset_opts_env(void) {
  for (size_t i = 0; i < LS_ARRAY_LEN(OPT_ENV_NAMES); i++) {
    (void)unsetenv(OPT_ENV_NAMES[i]);
  }
}

static void set_opts_env(const char *name, const char *value) {
  (void)setenv(name, value, 1);
}

static bool resolve_opts(int argc, char *argv[], ls_opts_t *opts,
                         bool *exit_requested, ls_err_t *err) {
  *opts = (ls_opts_t){};
  *err = (ls_err_t){};
  *exit_requested = false;
  return ls_opts_resolve(opts, argc, argv, exit_requested, err);
}

static bool host_has_systemd(void) {
  struct stat runtime;
  return access("/usr/bin/systemctl", X_OK) == 0 &&
         stat("/run/systemd/system", &runtime) == 0 &&
         S_ISDIR(runtime.st_mode);
}

static void expect_opts_reject(const char *needle, char *argv[], int argc) {
  ls_opts_t opts;
  bool exit_requested = false;
  ls_err_t err = {};
  CHECK(!resolve_opts(argc, argv, &opts, &exit_requested, &err));
  CHECK(!exit_requested);
  CHECK(strstr(err.msg, needle) != nullptr);
}

static void test_opts_defaults(void) {
  reset_opts_env();
  char *argv[] = {(char *)"linkstay"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK(!exit_requested);
  CHECK_STR(opts.target, "1.1.1.1");
  CHECK(opts.interval_ms == UINT64_C(10000));
  CHECK(opts.fail_threshold == 5);
  CHECK(opts.timeout_ms == UINT64_C(3000));
  CHECK(!opts.poweroff);
  CHECK(opts.log_level == LS_LOG_INFO);
  CHECK(opts.systemd);
}

static void test_opts_cli(void) {
  reset_opts_env();
  char *argv[] = {(char *)"linkstay", "-t", "8.8.8.8", "-i", "2",
                  "-n", "3", "-w", "500", "--poweroff=false",
                  "--log-level=DEBUG", "--systemd=FALSE"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK(!exit_requested);
  CHECK_STR(opts.target, "8.8.8.8");
  CHECK(opts.interval_ms == UINT64_C(2000));
  CHECK(opts.fail_threshold == 3);
  CHECK(opts.timeout_ms == UINT64_C(500));
  CHECK(!opts.poweroff);
  CHECK(opts.log_level == LS_LOG_DEBUG);
  CHECK(!opts.systemd);
}

static void test_opts_attached_forms(void) {
  reset_opts_env();
  char *argv[] = {(char *)"linkstay", "-t8.8.8.8", "-i2", "-n3", "-w500",
                  "-pfalse", "-ldebug", "-sfalse"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK_STR(opts.target, "8.8.8.8");
  CHECK(opts.interval_ms == UINT64_C(2000));
  CHECK(opts.fail_threshold == 3);
  CHECK(opts.timeout_ms == UINT64_C(500));
  CHECK(!opts.poweroff);
  CHECK(opts.log_level == LS_LOG_DEBUG);
  CHECK(!opts.systemd);
}

static void test_opts_duration_suffixes(void) {
  reset_opts_env();
  char *argv[] = {(char *)"linkstay", "-t", "1.1.1.1", "-i", "1500ms",
                  "-w", "1s"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK(opts.interval_ms == UINT64_C(1500));
  CHECK(opts.timeout_ms == UINT64_C(1000));
}

static void test_opts_env(void) {
  reset_opts_env();
  set_opts_env("LINKSTAY_TARGET", "8.8.8.8");
  set_opts_env("LINKSTAY_INTERVAL", "5");
  set_opts_env("LINKSTAY_THRESHOLD", "4");
  set_opts_env("LINKSTAY_TIMEOUT", "400");
  set_opts_env("LINKSTAY_POWEROFF", "false");
  set_opts_env("LINKSTAY_LOG_LEVEL", "warn");
  set_opts_env("LINKSTAY_SYSTEMD", "false");

  char *argv[] = {(char *)"linkstay"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK_STR(opts.target, "8.8.8.8");
  CHECK(opts.interval_ms == UINT64_C(5000));
  CHECK(opts.fail_threshold == 4);
  CHECK(opts.timeout_ms == UINT64_C(400));
  CHECK(!opts.poweroff);
  CHECK(opts.log_level == LS_LOG_WARN);
  CHECK(!opts.systemd);
}

static void test_opts_precedence(void) {
  reset_opts_env();
  set_opts_env("LINKSTAY_TARGET", "invalid");
  set_opts_env("LINKSTAY_INTERVAL", "abc");
  set_opts_env("LINKSTAY_POWEROFF", "junk");

  char *argv[] = {(char *)"linkstay", "-t", "1.1.1.1", "-i", "2",
                  "-w", "500", "-pfalse"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  CHECK(resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested,
                     &err));
  CHECK_STR(opts.target, "1.1.1.1");
  CHECK(opts.interval_ms == UINT64_C(2000));
  CHECK(!opts.poweroff);

  reset_opts_env();
  set_opts_env("LINKSTAY_INTERVAL", "abc");
  char *env_only[] = {(char *)"linkstay", "-t", "1.1.1.1"};
  CHECK(!resolve_opts((int)LS_ARRAY_LEN(env_only), env_only, &opts,
                      &exit_requested, &err));
  CHECK(strstr(err.msg, "Invalid value for LINKSTAY_INTERVAL") != nullptr);
}

static void test_opts_validation(void) {
  reset_opts_env();

  char *cross_field[] = {(char *)"linkstay", "-t", "1.1.1.1", "-i", "1",
                         "-w", "1000"};
  expect_opts_reject("must be smaller than interval", cross_field,
                     (int)LS_ARRAY_LEN(cross_field));

  char *hostname[] = {(char *)"linkstay", "-t", "example.com"};
  expect_opts_reject("valid IPv4 or IPv6", hostname,
                     (int)LS_ARRAY_LEN(hostname));

  char *empty_target[] = {(char *)"linkstay", "--target="};
  expect_opts_reject("Target host cannot be empty", empty_target,
                     (int)LS_ARRAY_LEN(empty_target));

  char *bad_level[] = {(char *)"linkstay", "-t", "1.1.1.1",
                       "--log-level=bogus"};
  expect_opts_reject("Invalid value for --log-level", bad_level,
                     (int)LS_ARRAY_LEN(bad_level));

  char *bad_bool[] = {(char *)"linkstay", "-t", "1.1.1.1", "--systemd=1"};
  expect_opts_reject("Invalid value for --systemd", bad_bool,
                     (int)LS_ARRAY_LEN(bad_bool));

  char *unknown[] = {(char *)"linkstay", "--does-not-exist"};
  expect_opts_reject("Unknown option", unknown, (int)LS_ARRAY_LEN(unknown));

  char *missing[] = {(char *)"linkstay", "-i"};
  expect_opts_reject("requires an argument", missing,
                     (int)LS_ARRAY_LEN(missing));

  char *stray[] = {(char *)"linkstay", "-t", "1.1.1.1", "stray"};
  expect_opts_reject("Unexpected argument", stray, (int)LS_ARRAY_LEN(stray));

  char *zero_interval[] = {(char *)"linkstay", "-t", "1.1.1.1", "-i", "0"};
  expect_opts_reject("Invalid value for --interval", zero_interval,
                     (int)LS_ARRAY_LEN(zero_interval));

  char *zero_threshold[] = {(char *)"linkstay", "-t", "1.1.1.1", "-n", "0"};
  expect_opts_reject("Invalid value for --threshold", zero_threshold,
                     (int)LS_ARRAY_LEN(zero_threshold));
}

static void test_opts_poweroff_runtime(void) {
  reset_opts_env();
  char *argv[] = {(char *)"linkstay", "-t", "1.1.1.1", "--poweroff=true"};
  ls_opts_t opts;
  bool exit_requested = true;
  ls_err_t err = {};

  bool resolved =
      resolve_opts((int)LS_ARRAY_LEN(argv), argv, &opts, &exit_requested, &err);
  if (host_has_systemd()) {
    CHECK(resolved);
    CHECK(opts.poweroff);
  } else {
    CHECK(!resolved);
    CHECK(strstr(err.msg, "poweroff requires") != nullptr);
  }
}

static void test_stats(void) {
  ls_stats_t stats;
  ls_stats_init(&stats);
  CHECK(stats.total == 0);
  CHECK(stats.ok == 0);
  CHECK(stats.failed == 0);
  CHECK(ls_stats_success_rate(&stats) == 0.0);
  CHECK(ls_stats_avg_latency_ms(&stats) == 0.0);
  CHECK(ls_stats_min_latency_ms(&stats) == 0.0);
  CHECK(ls_stats_max_latency_ms(&stats) == 0.0);

  ls_stats_add_ok(&stats, 10.0);
  ls_stats_add_ok(&stats, 20.0);
  CHECK(stats.total == 2);
  CHECK(stats.ok == 2);
  CHECK(stats.failed == 0);
  CHECK(ls_stats_success_rate(&stats) == 100.0);
  CHECK(ls_stats_avg_latency_ms(&stats) == 15.0);
  CHECK(ls_stats_min_latency_ms(&stats) == 10.0);
  CHECK(ls_stats_max_latency_ms(&stats) == 20.0);

  ls_stats_add_fail(&stats);
  CHECK(stats.total == 3);
  CHECK(stats.failed == 1);
  double rate = ls_stats_success_rate(&stats);
  CHECK(rate > 66.66 && rate < 66.67);

  ls_stats_add_ok(&stats, -5.0);
  CHECK(stats.latency_min_us == 0);
  CHECK(ls_stats_min_latency_ms(&stats) == 0.0);

  ls_stats_t saturated = {
      .total = UINT64_MAX,
      .ok = UINT64_MAX,
      .failed = UINT64_MAX,
      .latency_sum_us = UINT64_MAX,
      .latency_min_us = 0,
      .latency_max_us = UINT64_MAX,
      .started_at_ms = 0,
  };
  ls_stats_add_ok(&saturated, 1.0);
  CHECK(saturated.total == UINT64_MAX);
  CHECK(saturated.ok == UINT64_MAX);
  CHECK(saturated.latency_sum_us == UINT64_MAX);
  ls_stats_add_fail(&saturated);
  CHECK(saturated.failed == UINT64_MAX);

  ls_stats_t uptime = {.started_at_ms = UINT64_MAX};
  CHECK(ls_stats_uptime_sec(&uptime) == 0);
  uptime.started_at_ms = 0;
  CHECK(ls_stats_uptime_sec(&uptime) > 0);
}

static void test_log_level_names(void) {
  CHECK_STR(ls_log_level_name(LS_LOG_SILENT), "SILENT");
  CHECK_STR(ls_log_level_name(LS_LOG_ERROR), "ERROR");
  CHECK_STR(ls_log_level_name(LS_LOG_WARN), "WARN");
  CHECK_STR(ls_log_level_name(LS_LOG_INFO), "INFO");
  CHECK_STR(ls_log_level_name(LS_LOG_DEBUG), "DEBUG");
  CHECK_STR(ls_log_level_name((ls_log_level_t)99), "UNKNOWN");
}

int main(void) {
  test_error_type();
  test_saturating_math();
  test_duration_format();
  test_latency_format();
  test_uptime_format();
  test_addr_parse();
  test_timer_helpers();
  test_opts_defaults();
  test_opts_cli();
  test_opts_attached_forms();
  test_opts_duration_suffixes();
  test_opts_env();
  test_opts_precedence();
  test_opts_validation();
  test_opts_poweroff_runtime();
  test_stats();
  test_log_level_names();

  if (failures != 0) {
    (void)fprintf(stderr, "unit: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  (void)printf("unit: %d checks passed\n", checks);
  return 0;
}
