#!/usr/bin/env bash
# Live ICMP, signal, and threshold tests. Sourced by run_tests.sh; requires
# CAP_NET_RAW or LINKSTAY_TEST_LIVE=1 inside a user+network namespace.
TEST_DIR="${TEST_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)}"
section "Live ICMP (requires CAP_NET_RAW)"

caps="$(caps_str)"
if [[ "$caps" != *cap_net_raw* && "${LINKSTAY_TEST_LIVE:-0}" != "1" ]]; then
  skip "Live ICMP suite" "$BIN lacks cap_net_raw — run: sudo setcap cap_net_raw=eip $BIN (or LINKSTAY_TEST_LIVE=1 inside 'unshare -Urn')"
else
  # 1) Successful ping to loopback, SIGTERM after ~2.5s, expect clean exit + replies
  spawn env LINKSTAY_TARGET=127.0.0.1 LINKSTAY_INTERVAL=1s \
    LINKSTAY_TIMEOUT=500ms LINKSTAY_LOG_LEVEL=debug \
    LINKSTAY_POWEROFF=false "$BIN"
  sleep 2.5
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  hay="${STDOUT}${STDERR}"
  [[ "$RC" == "0" ]] && pass "loopback ping clean exit on SIGTERM" || \
    fail "loopback ping clean exit on SIGTERM" "rc=$RC"
  [[ "$hay" == *"Reply from 127.0.0.1"* ]] && pass "loopback received ICMP replies" || \
    fail "loopback received ICMP replies" "no 'Reply from 127.0.0.1' lines"
  [[ "$hay" == *"Statistics:"* ]] && pass "loopback prints final statistics" || \
    fail "loopback prints final statistics" "no Statistics line"
  [[ "$hay" == *"poweroff false (dry-run)"* ]] && \
    pass "startup banner spells out the dry-run mode" || \
    fail "startup banner spells out the dry-run mode" "no dry-run marker"
  [[ "$hay" == *"time="*"ms avg="*"ms ok="* ]] && \
    pass "reply line reports latency and running totals" || \
    fail "reply line reports latency and running totals" "unexpected reply format"
  # min <= avg <= max must hold for any set of samples.
  latency="$(grep -o 'latency min [0-9.]*ms, avg [0-9.]*ms, max [0-9.]*ms' <<<"$hay" | tail -1)"
  if awk -v line="$latency" 'BEGIN{
        gsub(/[^0-9. ]/, " ", line);
        n = split(line, v, " ");
        exit !(n == 3 && v[1] <= v[2] && v[2] <= v[3]);
      }'; then
    pass "latency statistics satisfy min <= avg <= max"
  else
    fail "latency statistics satisfy min <= avg <= max" "line=[$latency]"
  fi
  [[ "$hay" == *"ICMP BPF filter active"* ]] && pass "IPv4 BPF filter attached" || \
    fail "IPv4 BPF filter attached" "no BPF debug line"

  # 2) SIGINT is treated like SIGTERM
  spawn env LINKSTAY_TARGET=127.0.0.1 LINKSTAY_INTERVAL=1s \
    LINKSTAY_TIMEOUT=500ms LINKSTAY_LOG_LEVEL=info \
    LINKSTAY_POWEROFF=false "$BIN"
  sleep 1.5
  kill -INT "$PID" 2>/dev/null || true
  await 3
  hay="${STDOUT}${STDERR}"
  [[ "$RC" == "0" ]] && pass "SIGINT exits cleanly" || \
    fail "SIGINT exits cleanly" "rc=$RC"
  [[ "$hay" == *"Shutdown signal received"* ]] && pass "SIGINT logs shutdown" || \
    fail "SIGINT logs shutdown" "no shutdown banner"

  # 3) IPv6 loopback ping
  spawn env LINKSTAY_TARGET=::1 LINKSTAY_INTERVAL=1s \
    LINKSTAY_TIMEOUT=500ms LINKSTAY_LOG_LEVEL=debug \
    LINKSTAY_POWEROFF=false "$BIN"
  sleep 1.5
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  hay="${STDOUT}${STDERR}"
  [[ "$RC" == "0" ]] && pass "IPv6 loopback ping clean exit" || \
    fail "IPv6 loopback ping clean exit" "rc=$RC"
  [[ "$hay" == *"Reply from ::1"* ]] && pass "IPv6 loopback received replies" || \
    fail "IPv6 loopback received replies" "no IPv6 reply line"

  # 4) SIGUSR1 dumps stats mid-run
  spawn env LINKSTAY_TARGET=127.0.0.1 LINKSTAY_INTERVAL=1s \
    LINKSTAY_TIMEOUT=500ms LINKSTAY_LOG_LEVEL=info \
    LINKSTAY_POWEROFF=false "$BIN"
  sleep 2.5
  kill -USR1 "$PID" 2>/dev/null || true
  sleep 0.3
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  # The startup line is at INFO; mid-run stats triggered by SIGUSR1 are at
  # INFO too. Count "Statistics:" lines — should be >= 2 (mid-run + shutdown).
  count=$(grep -c "Statistics:" <<<"${STDOUT}${STDERR}" || true)
  [[ "$count" -ge 2 ]] && pass "SIGUSR1 dumps stats (count=$count)" || \
    fail "SIGUSR1 dumps stats" "Statistics: count=$count"

  # NOTIFY protocol cases live in their own file but run as part of this suite.
  # shellcheck source=notify.sh
  . "$TEST_DIR/notify.sh"

  # 5) Unreachable target with poweroff disabled reaches threshold and exits.
  # Both a silent timeout (routable TEST-NET) and an immediate send failure
  # (no route in a bare namespace) must count toward the failure threshold.
  scrub_env
  unreachable="${LINKSTAY_TEST_UNREACHABLE_TARGET:-192.0.2.1}"
  run_capture timeout 10s env LINKSTAY_TARGET="$unreachable" \
    LINKSTAY_INTERVAL=1s LINKSTAY_TIMEOUT=300ms LINKSTAY_THRESHOLD=2 \
    LINKSTAY_POWEROFF=false LINKSTAY_LOG_LEVEL=info "$BIN"
  hay="${STDOUT}${STDERR}"
  if [[ "$RC" == "0" && "$hay" == *"Dry-run complete"* ]]; then
    pass "dry-run threshold trigger exits cleanly"
  else
    fail "dry-run threshold trigger exits cleanly" "rc=$RC, hay tail=${hay: -300}"
  fi
fi
