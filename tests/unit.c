/*
 * unit.c — unit tests for the stateless helpers in base, addr, log, and loop.
 *
 * Built and run by `make unit-test`. No framework, no allocation: a failing
 * check prints its source location and the process exits non-zero.
 */

#include "addr.h"
#include "base.h"
#include "log.h"
#include "loop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

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
  test_log_level_names();

  if (failures != 0) {
    (void)fprintf(stderr, "unit: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  (void)printf("unit: %d checks passed\n", checks);
  return 0;
}
