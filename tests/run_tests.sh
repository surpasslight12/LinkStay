#!/usr/bin/env bash
# Comprehensive functional test for linkstay.
# - Static tests cover CLI/env parsing, validation, precedence, error messages.
# - Live tests require CAP_NET_RAW on $BIN; they are auto-skipped otherwise.
# - NOTIFY_SOCKET protocol tests use python3 when available; otherwise skipped.
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
     [[ "$hay" != *"Option requires an argument"* ]] && \
     [[ "$hay" != *"poweroff requires a systemd host"* ]]; then
    pass "$desc"
  else
    fail "$desc" "validation/parse error surfaced"
  fi
}

# accepts + debug output check: config resolves and the debug dump contains
# $needle. This works without CAP_NET_RAW because ls_opts_dump runs before the
# raw socket is opened.
expect_debug_contains() {
  local desc="$1" needle="$2"; shift 2
  scrub_env
  run_capture timeout --preserve-status -s TERM 1s "$@"
  local hay="${STDOUT}${STDERR}"
  if [[ "$hay" != *"Invalid value for"* ]] && \
     [[ "$hay" != *"Invalid IPv4/IPv6"* ]] && \
     [[ "$hay" != *"Target must be a valid IPv4 or IPv6"* ]] && \
     [[ "$hay" != *"Unknown option"* ]] && \
     [[ "$hay" != *"Unexpected argument"* ]] && \
     [[ "$hay" != *"Option requires an argument"* ]] && \
     [[ "$hay" != *"poweroff requires a systemd host"* ]] && \
     [[ "$hay" == *"$needle"* ]]; then
    pass "$desc"
  else
    fail "$desc" "missing needle: \"$needle\""
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


# Start a tiny AF_UNIX datagram receiver for NOTIFY_SOCKET protocol tests.
# Requires python3; prints one received datagram per line to $out.
NOTIFY_RECV_PID=""
start_notify_receiver() {
  local path="$1" out="$2"
  rm -f "$path"
  python3 - "$path" >"$out" 2>"$out.err" <<'PY' &
import socket, sys, time, os
path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
try:
    sock.bind(path)
    sock.settimeout(0.2)
    end = time.time() + 4.0
    while time.time() < end:
        try:
            data = sock.recv(1024)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        print(data.decode(errors="replace"), flush=True)
finally:
    sock.close()
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
PY
  NOTIFY_RECV_PID=$!
  for ((i = 0; i < 50; i++)); do
    [[ -S "$path" ]] && return 0
    sleep 0.05
  done
  return 1
}

stop_notify_receiver() {
  [[ -n "${NOTIFY_RECV_PID:-}" ]] && wait "$NOTIFY_RECV_PID" 2>/dev/null || true
  NOTIFY_RECV_PID=""
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

SYSTEMD_RUNTIME=0
if [[ -x /usr/bin/systemctl && -d /run/systemd/system ]]; then
  SYSTEMD_RUNTIME=1
fi

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

# help/version also win over invalid CLI arguments
run_capture "$BIN" --help --does-not-exist
assert_rc "--help wins over later unknown option" 0
run_capture "$BIN" --does-not-exist --help
assert_rc "--help wins over earlier unknown option" 0
assert_contains "--help after unknown option prints Usage" "Usage:"
run_capture "$BIN" --version --bad
assert_rc "--version wins over invalid option" 0

# combined short options resolve h/v normally
run_capture "$BIN" -vh
assert_rc "-vh exits 0" 0
assert_contains "-vh prints version" "version 1.0"
run_capture "$BIN" -hv
assert_rc "-hv exits 0" 0
assert_contains "-hv prints Usage" "Usage:"

# help text carries defaults, env names, and examples
scrub_env
run_capture "$BIN" --help
assert_contains "--help lists default target" "default: 1.1.1.1"
assert_contains "--help lists env variables" "LINKSTAY_TIMEOUT"
assert_contains "--help lists examples" "Examples:"

expect_reject "-- terminates option parsing" "Unexpected argument" \
  "$BIN" -- --help

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
expect_reject "rejects empty --target=" "Target host cannot be empty" \
  "$BIN" --target=
expect_reject "rejects overlong target" "too long" \
  "$BIN" -t "$(printf '1%.0s' {1..80})"
expect_accept "accepts IPv4 literal 1.1.1.1" "$BIN" -t 1.1.1.1
expect_accept "accepts attached -t1.1.1.1" "$BIN" -t1.1.1.1
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

# Attached short arguments for required numeric options
expect_accept "attached -i2" "$BIN" -t 1.1.1.1 -i2 -w 500 -s0 -l silent
expect_accept "attached -n3" "$BIN" -t 1.1.1.1 -n3 -w 500 -s0 -l silent
expect_accept "attached -w500" "$BIN" -t 1.1.1.1 -w500 -s0 -l silent
expect_accept "attached -ldebug" "$BIN" -t 1.1.1.1 -w500 -ldebug -s0

# Empty and junk values
expect_reject "--interval= empty" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 --interval=
expect_reject "--threshold= empty" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 --threshold=
expect_reject "--timeout= empty" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 --timeout=
expect_reject "--interval with trailing junk" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 5x -w 500
expect_reject "--timeout overflow" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 1 -w 999999999999999999999999

# Integer boundaries: parser accepts INT_MAX and rejects INT_MAX+1
expect_accept "--interval INT_MAX accepted" \
  "$BIN" -t 1.1.1.1 -i 2147483647 -s0 -l silent
expect_reject "--interval INT_MAX+1 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 2147483648
expect_accept "--threshold INT_MAX accepted" \
  "$BIN" -t 1.1.1.1 -n 2147483647 -s0 -l silent
expect_reject "--threshold INT_MAX+1 rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 -n 2147483648
expect_accept "--timeout INT_MAX accepted with matching interval" \
  "$BIN" -t 1.1.1.1 -i 2147484 -w 2147483647 -s0 -l silent
expect_reject "--timeout INT_MAX+1 rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 2147484 -w 2147483648

# strtoll permits a leading sign; document that behavior with acceptance tests
expect_accept "leading + on interval accepted" \
  "$BIN" -t 1.1.1.1 -i +5 -w 1000 -s0 -l silent
expect_accept "leading zeros accepted" \
  "$BIN" -t 1.1.1.1 -i 0005 -w 1000 -s0 -l silent

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
for v in false 0 no off; do
  expect_accept "--poweroff=$v" "$BIN" -t 1.1.1.1 --poweroff="$v"
done
expect_accept "--poweroff=false" "$BIN" -t 1.1.1.1 -p0
expect_accept "-pfalse" "$BIN" -t 1.1.1.1 -pfalse
expect_accept "-pNO (bool names are case-insensitive)" "$BIN" -t 1.1.1.1 -pNO
expect_accept "--poweroff omitted" "$BIN" -t 1.1.1.1
for v in true 1 yes on; do
  if (( SYSTEMD_RUNTIME )); then
    expect_accept "--poweroff=$v accepted on systemd host" \
      "$BIN" -t 1.1.1.1 --poweroff="$v"
  else
    expect_reject "--poweroff=$v rejected on non-systemd host" \
      "poweroff requires a systemd host" \
      "$BIN" -t 1.1.1.1 --poweroff="$v"
  fi
done
expect_reject "--poweroff invalid" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 --poweroff=bogus
expect_reject "--poweroff= empty" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 --poweroff=
expect_reject "--poweroff false is positional" "Unexpected argument" \
  "$BIN" -t 1.1.1.1 --poweroff false

# Bare -p enables poweroff. On systemd hosts validation passes and the binary
# enters the loop; elsewhere the validation error is the expected result.
scrub_env
run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 127.0.0.1 -p -l silent -s0
hay="${STDOUT}${STDERR}"
if (( SYSTEMD_RUNTIME )); then
  if [[ "$hay" != *"Invalid value for"* ]] \
     && [[ "$hay" != *"Unknown option"* ]] \
     && [[ "$hay" != *"poweroff requires a systemd host"* ]]; then
    pass "bare --poweroff accepted on systemd host"
  else
    fail "bare --poweroff accepted on systemd host" "hay=${hay: -200}"
  fi
else
  if [[ "$RC" != "0" && "$STDERR" == *"poweroff requires a systemd host"* ]]; then
    pass "bare --poweroff rejected on non-systemd host"
  else
    fail "bare --poweroff rejected on non-systemd host" "rc=$RC; hay=${hay: -200}"
  fi
fi

###############################################################################
section "Log-level parsing"
###############################################################################
for level in silent error warn info debug; do
  expect_accept "--log-level $level" "$BIN" -t 1.1.1.1 -l "$level"
done
expect_accept "case-insensitive --log-level=DEBUG" \
  "$BIN" -t 1.1.1.1 --log-level=DEBUG
expect_reject "--log-level bogus" "Invalid value for --log-level" \
  "$BIN" -t 1.1.1.1 -l bogus
expect_reject "--log-level empty" "Invalid value for --log-level" \
  "$BIN" -t 1.1.1.1 -l ''
expect_reject "--log-level= empty" "Invalid value for --log-level" \
  "$BIN" -t 1.1.1.1 --log-level=

scrub_env
run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l silent -s0
hay="${STDOUT}${STDERR}"
[[ -z "$hay" ]] && pass "silent suppresses even init errors" || \
  fail "silent suppresses even init errors" "hay=$hay"

###############################################################################
section "Bool / systemd flag parsing"
###############################################################################
for v in true 1 yes on false 0 no off; do
  expect_accept "--systemd=$v" "$BIN" -t 1.1.1.1 --systemd="$v"
done
expect_accept "--systemd=TRUE case-insensitive" \
  "$BIN" -t 1.1.1.1 --systemd=TRUE
expect_accept "bare --systemd" "$BIN" -t 1.1.1.1 --systemd
expect_accept "-s0 disables systemd" "$BIN" -t 1.1.1.1 -s0
expect_accept "-sfalse disables systemd" "$BIN" -t 1.1.1.1 -sfalse
expect_accept "-sFALSE case-insensitive" "$BIN" -t 1.1.1.1 -sFALSE
expect_accept "-sYES case-insensitive" "$BIN" -t 1.1.1.1 -sYES
expect_accept "bare -s enables systemd" "$BIN" -t 1.1.1.1 -s
expect_reject "--systemd=bogus rejected" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 --systemd=bogus
expect_reject "--systemd= empty rejected" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 --systemd=
expect_reject "--systemd false is positional" "Unexpected argument" \
  "$BIN" -t 1.1.1.1 --systemd false

###############################################################################
section "CLI parse errors"
###############################################################################
expect_reject "unknown long option" "Unknown option" \
  "$BIN" --does-not-exist
expect_reject "unknown short option" "Unknown option" \
  "$BIN" -Z
expect_reject "extra positional argument" "Unexpected argument" \
  "$BIN" -t 1.1.1.1 stray
for opt in --target --interval --threshold --timeout --log-level; do
  expect_reject "missing $opt argument" "requires an argument" "$BIN" "$opt"
done
for opt in -t -i -n -w -l; do
  expect_reject "missing $opt argument" "requires an argument" "$BIN" "$opt"
done

###############################################################################
section "Environment variables"
###############################################################################
# Each env var is accepted on its own
expect_accept "LINKSTAY_TARGET accepted" \
  env LINKSTAY_TARGET=8.8.8.8 "$BIN" -l silent -s0
expect_accept "LINKSTAY_INTERVAL accepted" \
  env LINKSTAY_INTERVAL=5 "$BIN" -t 1.1.1.1 -w 400 -s0 -l silent
expect_accept "LINKSTAY_THRESHOLD accepted" \
  env LINKSTAY_THRESHOLD=4 "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_TIMEOUT accepted" \
  env LINKSTAY_TIMEOUT=400 "$BIN" -t 1.1.1.1 -s0 -l silent
expect_accept "LINKSTAY_POWEROFF=false accepted" \
  env LINKSTAY_POWEROFF=false "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_LOG_LEVEL=debug accepted" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_SYSTEMD=false accepted" \
  env LINKSTAY_SYSTEMD=false "$BIN" -t 1.1.1.1

# Bool env vars accept every documented spelling, case-insensitively
for v in true 1 yes on false 0 no off; do
  if (( SYSTEMD_RUNTIME )); then
    expect_accept "LINKSTAY_POWEROFF=$v accepted" \
      env LINKSTAY_POWEROFF="$v" "$BIN" -t 1.1.1.1
  else
    case "$v" in
      true|1|yes|on)
        expect_reject "LINKSTAY_POWEROFF=$v rejected on non-systemd host" \
          "poweroff requires a systemd host" \
          env LINKSTAY_POWEROFF="$v" "$BIN" -t 1.1.1.1 ;;
      *)
        expect_accept "LINKSTAY_POWEROFF=$v accepted" \
          env LINKSTAY_POWEROFF="$v" "$BIN" -t 1.1.1.1 ;;
    esac
  fi
  expect_accept "LINKSTAY_SYSTEMD=$v accepted" \
    env LINKSTAY_SYSTEMD="$v" "$BIN" -t 1.1.1.1
done
expect_accept "LINKSTAY_SYSTEMD=TRUE case-insensitive" \
  env LINKSTAY_SYSTEMD=TRUE "$BIN" -t 1.1.1.1

# Invalid env values are rejected with the env var name
expect_reject "LINKSTAY_INTERVAL=abc rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=abc "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_THRESHOLD=abc rejected" "Invalid value for LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD=abc "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_TIMEOUT=0 rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=0 "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_POWEROFF=junk rejected" "Invalid value for LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=junk "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_LOG_LEVEL=junk rejected" "Invalid value for LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL=junk "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_SYSTEMD=junk rejected" "Invalid value for LINKSTAY_SYSTEMD" \
  env LINKSTAY_SYSTEMD=junk "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_INTERVAL= empty rejected" "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL= "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_THRESHOLD= empty rejected" "Invalid value for LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD= "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_TIMEOUT= empty rejected" "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT= "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_POWEROFF= empty rejected" "Invalid value for LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF= "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_LOG_LEVEL= empty rejected" "Invalid value for LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL= "$BIN" -t 1.1.1.1
expect_reject "LINKSTAY_SYSTEMD= empty rejected" "Invalid value for LINKSTAY_SYSTEMD" \
  env LINKSTAY_SYSTEMD= "$BIN" -t 1.1.1.1

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
expect_accept "CLI --threshold overrides invalid LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD=foo "$BIN" -t 1.1.1.1 -n 3 -w 500
expect_accept "CLI --timeout overrides invalid LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=foo "$BIN" -t 1.1.1.1 -w 500
expect_accept "CLI --poweroff overrides invalid LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=junk "$BIN" -t 1.1.1.1 -p0 -w 500
expect_accept "CLI --log-level overrides invalid LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL=junk "$BIN" -t 1.1.1.1 -l debug -w 500 -s0
expect_accept "CLI --systemd overrides invalid LINKSTAY_SYSTEMD" \
  env LINKSTAY_SYSTEMD=junk "$BIN" -t 1.1.1.1 -s0 -w 500

###############################################################################
section "Debug config dump"
###############################################################################
# The debug dump is emitted before the raw socket is opened, so it can be
# checked without CAP_NET_RAW.
expect_debug_contains "dump shows target" "Target: 8.8.8.8" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "dump shows interval" "Interval: 7 seconds" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "dump shows threshold" "Threshold: 4" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "dump shows timeout" "Timeout: 600 ms" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "dump shows poweroff false" "Poweroff: false" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "dump shows log level" "Log Level: DEBUG" \
  "$BIN" -t 8.8.8.8 -i 7 -w 600 -n 4 -p0 -l debug -s0
expect_debug_contains "systemd=false enables timestamps" "Timestamp: true" \
  "$BIN" -t 8.8.8.8 -l debug -s0
expect_debug_contains "systemd=true disables timestamps" "Timestamp: false" \
  "$BIN" -t 8.8.8.8 -l debug --systemd

# CLI values win over valid env values for every option; the debug dump lets
# us observe the final resolved configuration.
expect_debug_contains "CLI target wins over valid env target" "Target: 1.1.1.1" \
  env LINKSTAY_TARGET=8.8.8.8 \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI interval wins over valid env interval" "Interval: 2 seconds" \
  env LINKSTAY_INTERVAL=9 \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI timeout wins over valid env timeout" "Timeout: 500 ms" \
  env LINKSTAY_TIMEOUT=900 \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI threshold wins over valid env threshold" "Threshold: 3" \
  env LINKSTAY_THRESHOLD=9 \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI poweroff wins over valid env poweroff" "Poweroff: false" \
  env LINKSTAY_POWEROFF=true \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI log-level wins over valid env log-level" "Log Level: DEBUG" \
  env LINKSTAY_LOG_LEVEL=error \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0
expect_debug_contains "CLI systemd wins over valid env systemd" "Systemd: false" \
  env LINKSTAY_SYSTEMD=true \
  "$BIN" -t 1.1.1.1 -i 2 -w 500 -n 3 -p0 -l debug -s0

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
  [[ "$hay" == *"ICMP BPF filter active"* ]] && pass "IPv4 BPF filter attached" || \
    fail "IPv4 BPF filter attached" "no BPF debug line"
  [[ "$hay" == *"systemd integration inactive"* ]] && \
    pass "systemd integration inactive without NOTIFY_SOCKET" || \
    fail "systemd integration inactive without NOTIFY_SOCKET" "no inactive debug line"

  # 2) SIGINT is treated like SIGTERM
  spawn "$BIN" -t 127.0.0.1 -i 1 -w 500 -l info -s0
  sleep 1.5
  kill -INT "$PID" 2>/dev/null || true
  await 3
  hay="${STDOUT}${STDERR}"
  [[ "$RC" == "0" ]] && pass "SIGINT exits cleanly" || \
    fail "SIGINT exits cleanly" "rc=$RC"
  [[ "$hay" == *"Shutdown signal received"* ]] && pass "SIGINT logs shutdown" || \
    fail "SIGINT logs shutdown" "no shutdown banner"

  # 3) IPv6 loopback ping
  spawn "$BIN" -t ::1 -i 1 -w 500 -l debug -s0
  sleep 1.5
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  hay="${STDOUT}${STDERR}"
  [[ "$RC" == "0" ]] && pass "IPv6 loopback ping clean exit" || \
    fail "IPv6 loopback ping clean exit" "rc=$RC"
  [[ "$hay" == *"Reply from ::1"* ]] && pass "IPv6 loopback received replies" || \
    fail "IPv6 loopback received replies" "no IPv6 reply line"

  # 4) SIGUSR1 dumps stats mid-run
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

  # 5) NOTIFY_SOCKET protocol: READY, STATUS, WATCHDOG, STOPPING
  if command -v python3 >/dev/null 2>&1; then
    notify_sock="$TMP/notify.sock"
    notify_out="$TMP/notify.out"
    start_notify_receiver "$notify_sock" "$notify_out"
    if [[ -S "$notify_sock" ]]; then
      spawn env NOTIFY_SOCKET="$notify_sock" WATCHDOG_USEC=1000000 \
        "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug
      sleep 1.8
      kill -TERM "$PID" 2>/dev/null || true
      await 3
      stop_notify_receiver
      hay="${STDOUT}${STDERR}"
      messages="$(cat "$notify_out" 2>/dev/null || true)"
      [[ "$hay" == *"systemd integration active, watchdog ping every 500ms"* ]] && \
        pass "systemd watchdog interval derived from WATCHDOG_USEC" || \
        fail "systemd watchdog interval derived from WATCHDOG_USEC" \
          "missing watchdog debug line; hay tail=${hay: -300}"
      [[ "$messages" == *"READY=1"* ]] && pass "notify sends READY=1" || \
        fail "notify sends READY=1" "messages=$messages"
      [[ "$messages" == *"STATUS="* ]] && pass "notify sends STATUS=" || \
        fail "notify sends STATUS=" "messages=$messages"
      [[ "$messages" == *"WATCHDOG=1"* ]] && pass "notify sends WATCHDOG=1" || \
        fail "notify sends WATCHDOG=1" "messages=$messages"
      [[ "$messages" == *"STOPPING=1"* ]] && pass "notify sends STOPPING=1" || \
        fail "notify sends STOPPING=1" "messages=$messages"
      rm -f "$notify_sock" "$notify_out" "$notify_out.err"
    else
      stop_notify_receiver
      skip "NOTIFY_SOCKET protocol" "python3 receiver failed to bind $notify_sock"
      rm -f "$notify_sock" "$notify_out" "$notify_out.err"
    fi
  else
    skip "NOTIFY_SOCKET protocol" "python3 not available"
  fi

  # 6) WATCHDOG_PID mismatch disables watchdog while READY/STOPPING still work
  if command -v python3 >/dev/null 2>&1; then
    notify_sock="$TMP/notify-pid.sock"
    notify_out="$TMP/notify-pid.out"
    start_notify_receiver "$notify_sock" "$notify_out"
    if [[ -S "$notify_sock" ]]; then
      spawn env NOTIFY_SOCKET="$notify_sock" WATCHDOG_USEC=1000000 \
        WATCHDOG_PID=99999999 "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug
      sleep 1.2
      kill -TERM "$PID" 2>/dev/null || true
      await 3
      stop_notify_receiver
      hay="${STDOUT}${STDERR}"
      messages="$(cat "$notify_out" 2>/dev/null || true)"
      [[ "$hay" == *"watchdog disabled"* ]] && \
        pass "WATCHDOG_PID mismatch disables watchdog" || \
        fail "WATCHDOG_PID mismatch disables watchdog" "hay tail=${hay: -300}"
      [[ "$messages" == *"READY=1"* ]] && pass "notify READY works with WATCHDOG_PID set" || \
        fail "notify READY works with WATCHDOG_PID set" "messages=$messages"
      [[ "$messages" != *"WATCHDOG=1"* ]] && pass "no WATCHDOG with mismatched WATCHDOG_PID" || \
        fail "no WATCHDOG with mismatched WATCHDOG_PID" "messages=$messages"
      [[ "$messages" == *"STOPPING=1"* ]] && pass "notify STOPPING works with WATCHDOG_PID set" || \
        fail "notify STOPPING works with WATCHDOG_PID set" "messages=$messages"
      rm -f "$notify_sock" "$notify_out" "$notify_out.err"
    else
      stop_notify_receiver
      skip "WATCHDOG_PID mismatch notify" "python3 receiver failed to bind $notify_sock"
      rm -f "$notify_sock" "$notify_out" "$notify_out.err"
    fi
  else
    skip "WATCHDOG_PID mismatch notify" "python3 not available"
  fi

  # 7) Unreachable target with poweroff disabled reaches threshold and exits
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
