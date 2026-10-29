#!/usr/bin/env bash
# Environment configuration, validation, and systemd-only startup tests.
# Sourced by run_tests.sh; uses helpers from lib.sh.

section "Environment defaults"

expect_debug_contains "default target" "Target: 1.1.1.1" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "default interval" "Interval: 10s" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "default threshold" "Threshold: 5" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "default timeout" "Timeout: 3s" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "default poweroff" "Poweroff: false" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "default log level" "Log Level: DEBUG" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"

section "Environment overrides"

expect_debug_contains "target override" "Target: 8.8.8.8" \
  env LINKSTAY_TARGET=8.8.8.8 LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "interval override" "Interval: 2s" \
  env LINKSTAY_INTERVAL=2s LINKSTAY_TIMEOUT=500ms \
  LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "threshold override" "Threshold: 3" \
  env LINKSTAY_THRESHOLD=3 LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "timeout override" "Timeout: 500ms" \
  env LINKSTAY_TIMEOUT=500ms LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "poweroff override" "Poweroff: true" \
  env LINKSTAY_POWEROFF=true LINKSTAY_LOG_LEVEL=debug "$BIN"
expect_debug_contains "log-level debug enables the dump" "Log Level: DEBUG" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN"

section "Systemd-only startup"

expect_reject "NOTIFY_SOCKET is required" "NOTIFY_SOCKET" \
  env -u NOTIFY_SOCKET "$BIN"
expect_reject "unusable NOTIFY_SOCKET is fatal" "NOTIFY_SOCKET" \
  env NOTIFY_SOCKET="$TMP/absent-notify.sock" "$BIN"

section "Target validation"

expect_reject "hostname rejected (DNS disabled)" "valid IPv4 or IPv6" \
  env LINKSTAY_TARGET=example.com "$BIN"
expect_reject "malformed IPv4 rejected" "valid IPv4 or IPv6" \
  env LINKSTAY_TARGET=999.999.999.999 "$BIN"
expect_reject "malformed IPv6 rejected" "valid IPv4 or IPv6" \
  env LINKSTAY_TARGET='::g' "$BIN"
expect_reject "empty target rejected" "Target host cannot be empty" \
  env LINKSTAY_TARGET= "$BIN"

section "Numeric validation"

expect_reject "interval zero rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=0s "$BIN"
expect_reject "negative interval rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=-1s "$BIN"
expect_reject "bare interval rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=2 "$BIN"
expect_reject "leading plus rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=+5s "$BIN"
expect_reject "uppercase suffix rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=5S "$BIN"
expect_reject "timeout zero rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=0ms "$BIN"
expect_reject "timeout fraction rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=1.5s "$BIN"
expect_reject "timeout unknown unit rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=1m "$BIN"
expect_reject "threshold zero rejected" "Invalid value for LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD=0 "$BIN"
expect_reject "threshold junk rejected" "Invalid value for LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD=xyz "$BIN"
expect_reject "boolean alias rejected" "Invalid value for LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=1 "$BIN"
expect_reject "log-level junk rejected" "Invalid value for LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL=bogus "$BIN"

section "Bounds and cross-field validation"

expect_accept "interval INT_MAX accepted" \
  env LINKSTAY_INTERVAL=2147483647s LINKSTAY_LOG_LEVEL=silent "$BIN"
expect_reject "interval INT_MAX+1 rejected" \
  "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=2147483648s "$BIN"
expect_accept "timeout INT_MAX accepted" \
  env LINKSTAY_INTERVAL=2147484s LINKSTAY_TIMEOUT=2147483647ms \
  LINKSTAY_LOG_LEVEL=silent "$BIN"
expect_reject "timeout INT_MAX+1 rejected" \
  "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_INTERVAL=2147484s LINKSTAY_TIMEOUT=2147483648ms "$BIN"
expect_reject "timeout must be smaller than interval" \
  "must be smaller than interval" \
  env LINKSTAY_INTERVAL=1s LINKSTAY_TIMEOUT=1000ms "$BIN"
expect_accept "timeout smaller than interval accepted" \
  env LINKSTAY_INTERVAL=2s LINKSTAY_TIMEOUT=1500ms \
  LINKSTAY_LOG_LEVEL=silent "$BIN"

section "Poweroff runtime validation"

if (( SYSTEMD_RUNTIME )); then
  expect_accept "poweroff=true accepted on systemd host" \
    env LINKSTAY_POWEROFF=true LINKSTAY_LOG_LEVEL=silent "$BIN"
else
  expect_reject "poweroff=true rejected without systemd" \
    "poweroff requires a systemd host" \
    env LINKSTAY_POWEROFF=true "$BIN"
fi
