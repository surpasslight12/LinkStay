#!/usr/bin/env bash
# Comprehensive functional test for linkstay.
# - Static tests cover CLI/env parsing, validation, precedence, error messages.
# - Live tests require CAP_NET_RAW on $BIN; they are auto-skipped otherwise.
# Exits non-zero if any test failed.

set -u

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/bin/linkstay}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
SKIP=0
declare -a FAILED

if [[ -t 1 ]]; then
  G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; B=$'\033[34m'; X=$'\033[0m'
else
  G=""; R=""; Y=""; B=""; X=""
fi

scrub_env() {
  unset LINKSTAY_TARGET LINKSTAY_INTERVAL LINKSTAY_THRESHOLD \
        LINKSTAY_TIMEOUT LINKSTAY_POWEROFF \
        LINKSTAY_LOG_LEVEL LINKSTAY_SYSTEMD \
        NOTIFY_SOCKET WATCHDOG_USEC WATCHDOG_PID
}

# Run "$@" capturing stdout/stderr/exit into RC/STDOUT/STDERR.
# Honors any extra LINKSTAY_*/NOTIFY_* env supplied via env_<X>=Y prefix list
# passed in the call wrapper.
run_capture() {
  : >"$TMP/out"
  : >"$TMP/err"
  "$@" >"$TMP/out" 2>"$TMP/err"
  RC=$?
  STDOUT="$(cat "$TMP/out")"
  STDERR="$(cat "$TMP/err")"
}

pass() { PASS=$((PASS+1)); printf "  %s✓%s %s\n" "$G" "$X" "$1"; }
fail() {
  FAIL=$((FAIL+1)); FAILED+=("$1")
  printf "  %s✗%s %s\n" "$R" "$X" "$1"
  [[ ${2-} ]] && printf "      %s\n" "$2"
  [[ ${STDERR-} ]] && printf "      stderr: %s\n" "${STDERR:0:300}"
  [[ ${STDOUT-} ]] && [[ -z "${STDERR-}" || "${STDOUT}" != "" ]] && \
    printf "      stdout: %s\n" "${STDOUT:0:300}"
}
skip() { SKIP=$((SKIP+1)); printf "  %s-%s %s (skipped: %s)\n" "$Y" "$X" "$1" "${2-}"; }
section() { printf "\n%s== %s ==%s\n" "$B" "$1" "$X"; }

assert_rc() {
  local desc="$1" expected="$2"
  if [[ "$RC" == "$expected" ]]; then pass "$desc"
  else fail "$desc" "rc=$RC, expected $expected"; fi
}

assert_contains() {
  local desc="$1" needle="$2" hay="${STDOUT}${STDERR}"
  if [[ "$hay" == *"$needle"* ]]; then pass "$desc"
  else fail "$desc" "missing: \"$needle\""; fi
}

assert_not_contains() {
  local desc="$1" needle="$2" hay="${STDOUT}${STDERR}"
  if [[ "$hay" != *"$needle"* ]]; then pass "$desc"
  else fail "$desc" "unexpected: \"$needle\""; fi
}

# rejects: command exits non-zero with an [ERROR] line containing needle
expect_reject() {
  local desc="$1" needle="$2"; shift 2
  scrub_env
  run_capture "$@"
  if [[ "$RC" != "0" && "$STDERR" == *"[ERROR]"* && "$STDERR" == *"$needle"* ]]; then
    pass "$desc"
  else
    fail "$desc" "rc=$RC; needle=\"$needle\""
  fi
}

# accepts: command should pass ls_opts_resolve. We wrap in `timeout` because a
# successful resolve will enter the event loop and run forever; any non-parse
# exit (incl. timeout SIGTERM) is fine. We assert the absence of the
# parse/validation error markers in the captured output.
expect_accept() {
  local desc="$1"; shift
  scrub_env
  run_capture timeout --preserve-status -s TERM 1s "$@"
  local hay="${STDOUT}${STDERR}"
  if [[ "$hay" != *"Invalid value for"* ]] && \
     [[ "$hay" != *"Invalid IPv4/IPv6"* ]] && \
     [[ "$hay" != *"Target must be a valid IPv4 or IPv6"* ]] && \
     [[ "$hay" != *"Unknown option"* ]] && \
     [[ "$hay" != *"Unexpected argument"* ]] && \
     [[ "$hay" != *"Option requires an argument"* ]]; then
    pass "$desc"
  else
    fail "$desc" "validation/parse error surfaced"
  fi
}

# Spawn binary as a direct child of the current shell so `wait` works.
# Sets globals PID, SPAWN_OUT, SPAWN_ERR.
spawn() {
  scrub_env
  SPAWN_OUT="$TMP/spawn.out.$$.$RANDOM"
  SPAWN_ERR="$TMP/spawn.err.$$.$RANDOM"
  : >"$SPAWN_OUT"
  : >"$SPAWN_ERR"
  "$@" >"$SPAWN_OUT" 2>"$SPAWN_ERR" &
  PID=$!
}

# Wait for PID up to `wait_secs`; if still alive send SIGTERM and wait once
# more. Populates RC/STDOUT/STDERR from the spawn files.
await() {
  local wait_secs=${1-5}
  for ((i=0; i<wait_secs*10; i++)); do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    sleep 0.1
  done
  if kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    for ((i=0; i<30; i++)); do
      kill -0 "$PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "$PID" 2>/dev/null || true
  fi
  wait "$PID" 2>/dev/null
  RC=$?
  STDOUT="$(cat "$SPAWN_OUT" 2>/dev/null || true)"
  STDERR="$(cat "$SPAWN_ERR" 2>/dev/null || true)"
  rm -f "$SPAWN_OUT" "$SPAWN_ERR"
}

if [[ ! -x "$BIN" ]]; then
  echo "Error: binary not found at $BIN. Run 'make' first." >&2
  exit 2
fi

# getcap may live in /sbin which is not always in PATH
GETCAP=""
for p in getcap /sbin/getcap /usr/sbin/getcap; do
  if command -v "$p" >/dev/null 2>&1; then GETCAP="$p"; break; fi
done
caps_str() { [[ -n "$GETCAP" ]] && "$GETCAP" "$BIN" 2>/dev/null || true; }

printf "Test target: %s\n" "$BIN"
printf "Capabilities: %s\n" "$(caps_str || echo none)"

###############################################################################
section "Help & version"
###############################################################################
scrub_env
run_capture "$BIN" --version; assert_rc "--version exits 0" 0
assert_contains "--version prints version" "version 1.0"

scrub_env
run_capture "$BIN" -v; assert_rc "-v exits 0" 0

scrub_env
run_capture "$BIN" --help; assert_rc "--help exits 0" 0
assert_contains "--help prints Usage" "Usage:"
assert_contains "--help lists -t/--target" "--target"
assert_contains "--help lists -n/--threshold" "--threshold"
assert_contains "--help lists -s/--systemd" "--systemd"

scrub_env
run_capture "$BIN" -h; assert_rc "-h exits 0" 0

# --help short-circuits even when env is invalid
LINKSTAY_TARGET=notanip run_capture "$BIN" --help
assert_rc "--help short-circuits invalid env" 0
LINKSTAY_THRESHOLD=abc run_capture "$BIN" --version
assert_rc "--version short-circuits invalid env" 0

###############################################################################
section "Target IP literal validation"
###############################################################################
expect_reject "rejects hostname (DNS disabled)" "must be a valid IPv4 or IPv6" \
  "$BIN" -t example.com
expect_reject "rejects malformed IPv4" "must be a valid IPv4 or IPv6" \
  "$BIN" -t 999.999.999.999
expect_reject "rejects malformed IPv6" "must be a valid IPv4 or IPv6" \
  "$BIN" -t '::g'
expect_reject "rejects empty target" "Target host cannot be empty" \
  env LINKSTAY_TARGET= "$BIN"
expect_accept "accepts IPv4 literal 1.1.1.1" "$BIN" -t 1.1.1.1
expect_accept "accepts IPv4 literal 127.0.0.1" "$BIN" -t 127.0.0.1
expect_accept "accepts IPv6 literal 2606:4700:4700::1111" \
  "$BIN" -t 2606:4700:4700::1111
expect_accept "accepts IPv6 literal ::1" "$BIN" -t ::1

###############################################################################
section "Numeric arg validation"
###############################################################################
expect_reject "--interval 0 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 0
expect_reject "--interval -1 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i -1
expect_reject "--interval abc rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i abc
expect_reject "--threshold 0 rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 -n 0
expect_reject "--threshold xyz rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 --threshold xyz
expect_reject "--timeout 0 rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -w 0
expect_reject "--timeout negative rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -w -100
expect_accept "--interval 1 / --threshold 1 / --timeout 500 accepted" \
  "$BIN" -t 1.1.1.1 -i 1 -n 1 -w 500

###############################################################################
section "Cross-field validation"
###############################################################################
expect_reject "timeout > interval rejected" "must be smaller than interval" \
  "$BIN" -t 1.1.1.1 -i 1 -w 5000
expect_reject "timeout == interval rejected" "must be smaller than interval" \
  "$BIN" -t 1.1.1.1 -i 1 -w 1000
expect_accept "timeout < interval accepted" "$BIN" -t 1.1.1.1 -i 2 -w 1500
expect_accept "timeout just below interval accepted (no hidden margin)" \
  "$BIN" -t 1.1.1.1 -i 1 -w 999

###############################################################################
section "Poweroff parsing"
###############################################################################
expect_accept "--poweroff=false" "$BIN" -t 1.1.1.1 -p0
expect_accept "--poweroff omitted" "$BIN" -t 1.1.1.1
expect_reject "--poweroff invalid" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 --poweroff=bogus
# poweroff=true requires systemctl + /run/systemd/system. On hosts where these
# are present, options validation passes and the binary then enters the event
# loop. We wrap in a short timeout and ping loopback so failures never
# accumulate (no real shutdown can fire within the window). Either outcome
# confirms the config-layer behavior.
scrub_env
run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 127.0.0.1 -p -l silent -s0
hay="${STDOUT}${STDERR}"
if [[ "$hay" != *"Invalid value for"* ]] \
   && [[ "$hay" != *"Unknown option"* ]]; then
  pass "--poweroff accepted by config"
else
  fail "--poweroff unexpected outcome" "rc=$RC; hay=${hay: -200}"
fi

###############################################################################
section "Log-level parsing"
###############################################################################
for level in silent error warn info debug; do
  expect_accept "--log-level $level" "$BIN" -t 1.1.1.1 -l "$level"
done
expect_reject "--log-level bogus" "Invalid value for --log-level" \
  "$BIN" -t 1.1.1.1 -l bogus
expect_reject "--log-level empty" "Invalid value for --log-level" \
  "$BIN" -t 1.1.1.1 -l ''

###############################################################################
section "Bool / systemd flag parsing"
###############################################################################
for v in true 1 yes on; do
  expect_accept "--systemd=$v" "$BIN" -t 1.1.1.1 --systemd="$v"
done
for v in false 0 no off; do
  expect_accept "--systemd=$v" "$BIN" -t 1.1.1.1 --systemd="$v"
done
expect_accept "bare --systemd" "$BIN" -t 1.1.1.1 --systemd
expect_accept "-s0 disables systemd" "$BIN" -t 1.1.1.1 -s0
expect_accept "-sfalse disables systemd" "$BIN" -t 1.1.1.1 -sfalse
expect_accept "bare -s enables systemd" "$BIN" -t 1.1.1.1 -s
expect_reject "--systemd=bogus rejected" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 --systemd=bogus

###############################################################################
section "CLI parse errors"
###############################################################################
expect_reject "unknown long option" "Unknown option" \
  "$BIN" --does-not-exist
expect_reject "extra positional argument" "Unexpected argument" \
  "$BIN" -t 1.1.1.1 stray
expect_reject "missing required arg" "requires an argument" \
  "$BIN" -t

###############################################################################
section "Environment variables"
###############################################################################
# Each env var individually accepted
LINKSTAY_TARGET=8.8.8.8 run_capture timeout --preserve-status -s TERM 1s "$BIN" -l silent -s0; \
  [[ "$STDERR" != *"Invalid IPv4/IPv6"* && "$STDERR" != *"Target must be a valid"* ]] && \
    pass "LINKSTAY_TARGET accepted" || \
    fail "LINKSTAY_TARGET accepted" "$STDERR"

LINKSTAY_INTERVAL=5 LINKSTAY_TIMEOUT=400 run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l silent -s0
[[ "$STDERR" != *"Invalid value for LINKSTAY_INTERVAL"* && \
   "$STDERR" != *"Invalid value for LINKSTAY_TIMEOUT"* ]] && \
  pass "LINKSTAY_INTERVAL+LINKSTAY_TIMEOUT accepted" || \
  fail "LINKSTAY_INTERVAL+LINKSTAY_TIMEOUT accepted" "$STDERR"

expect_reject "LINKSTAY_INTERVAL=abc rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=abc "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_TIMEOUT=0 rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=0 "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_POWEROFF=junk rejected" "Invalid value for LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=junk "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_LOG_LEVEL=junk rejected" "Invalid value for LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL=junk "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_SYSTEMD=junk rejected" "Invalid value for LINKSTAY_SYSTEMD" \
  env LINKSTAY_SYSTEMD=junk "$BIN" -t 1.1.1.1

# Threshold env var
expect_accept "LINKSTAY_THRESHOLD alone" \
  env LINKSTAY_THRESHOLD=4 "$BIN" -t 1.1.1.1

###############################################################################
section "Env / CLI precedence"
###############################################################################
# CLI must override env: env invalid target overridden by valid CLI target
scrub_env
LINKSTAY_TARGET=invalid run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l silent -s0
[[ "$STDERR" != *"Invalid IPv4/IPv6"* && "$STDERR" != *"Target must be a valid"* ]] && \
  pass "CLI --target overrides invalid LINKSTAY_TARGET" || \
  fail "CLI --target overrides invalid LINKSTAY_TARGET" "$STDERR"

# Env invalid value still rejected when CLI does not override it
expect_reject "env-invalid surfaces when CLI does not override" \
  "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=foo "$BIN" -t 1.1.1.1

# CLI must override even an invalid env value, not just an invalid string value
expect_accept "CLI --interval overrides invalid LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=foo "$BIN" -t 1.1.1.1 -i 2 -w 500
expect_accept "CLI --poweroff overrides invalid LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=junk "$BIN" -t 1.1.1.1 -p0 -w 500

###############################################################################
section "Debug config dump"
###############################################################################
# Start the binary briefly so ls_app_init() emits the debug configuration
# dump before socket setup fails without CAP_NET_RAW.
scrub_env
run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l debug -s0
hay="${STDOUT}${STDERR}"
if [[ "$hay" == *"Configuration:"* && "$hay" == *"Target: 1.1.1.1"* ]]; then
  pass "--log-level=debug prints Configuration:"
else
  # If init exited before reaching ls_opts_dump (shouldn't happen) note it
  fail "--log-level=debug prints Configuration:" "no Configuration block"
fi

###############################################################################
section "Live ICMP (requires CAP_NET_RAW)"
###############################################################################
caps="$(caps_str)"
if [[ "$caps" != *cap_net_raw* ]]; then
  skip "Live ICMP suite" "$BIN lacks cap_net_raw — run: sudo setcap cap_net_raw=eip $BIN"
else
  # 1) Successful ping to loopback, SIGTERM after ~2.5s, expect clean exit + replies
  spawn "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug -s0
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

  # 2) SIGUSR1 dumps stats mid-run
  spawn "$BIN" -t 127.0.0.1 -i 1 -w 500 -l info -s0
  sleep 2.5
  kill -USR1 "$PID" 2>/dev/null || true
  sleep 0.3
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  # The startup line is at INFO; mid-run stats triggered by SIGUSR1 are at INFO
  # too. Count "Statistics:" lines — should be >= 2 (mid-run + shutdown).
  count=$(grep -c "Statistics:" <<<"${STDOUT}${STDERR}" || true)
  [[ "$count" -ge 2 ]] && pass "SIGUSR1 dumps stats (count=$count)" || \
    fail "SIGUSR1 dumps stats" "Statistics: count=$count"

  # 3) Unreachable target with poweroff disabled reaches threshold and exits
  scrub_env
  # 192.0.2.0/24 is TEST-NET-1 (RFC 5737), guaranteed not routable
  run_capture timeout 10s "$BIN" -t 192.0.2.1 -i 1 -w 300 -n 2 -l info -s0
  hay="${STDOUT}${STDERR}"
  if [[ "$RC" == "0" && "$hay" == *"Dry-run complete"* ]]; then
    pass "dry-run threshold trigger exits cleanly"
  else
    fail "dry-run threshold trigger exits cleanly" "rc=$RC, hay tail=${hay: -300}"
  fi
fi

###############################################################################
section "Summary"
###############################################################################
printf "Pass: %d   Fail: %d   Skip: %d\n" "$PASS" "$FAIL" "$SKIP"
if (( FAIL > 0 )); then
  printf "\n%sFailed tests:%s\n" "$R" "$X"
  for t in "${FAILED[@]}"; do printf "  - %s\n" "$t"; done
  exit 1
fi
exit 0
