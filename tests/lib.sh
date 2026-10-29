#!/usr/bin/env bash
# Shared assertions, process helpers, and NOTIFY receiver helpers for the
# linkstay test suite. Sourced by run_tests.sh; not run directly.
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
        LINKSTAY_TIMEOUT LINKSTAY_START_DELAY LINKSTAY_POWEROFF \
        LINKSTAY_LOG_LEVEL NOTIFY_SOCKET WATCHDOG_USEC WATCHDOG_PID
  if [[ -n "${TEST_NOTIFY_SOCKET:-}" ]]; then
    export NOTIFY_SOCKET="$TEST_NOTIFY_SOCKET"
  fi
}

# Run "$@" capturing stdout/stderr/exit into RC/STDOUT/STDERR.
# Extra LINKSTAY_*/NOTIFY_* variables are supplied by callers as
# `env VAR=value "$BIN" ...`.
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

# True when captured output contains no configuration/validation error markers.
no_config_error() {
  local hay="$1"
  [[ "$hay" != *"Invalid value for"* ]] && \
  [[ "$hay" != *"Invalid IPv4/IPv6"* ]] && \
  [[ "$hay" != *"Target must be a valid IPv4 or IPv6"* ]] && \
  [[ "$hay" != *"poweroff requires a systemd host"* ]] && \
  [[ "$hay" != *"NOTIFY_SOCKET"* ]]
}

# accepts: command should pass ls_opts_resolve. We wrap in `timeout` because a
# successful resolve will enter the event loop and run forever; any
# non-validation exit (including timeout SIGTERM) is fine. We assert the absence
# of configuration/validation error markers in the captured output.
expect_accept() {
  local desc="$1"; shift
  scrub_env
  run_capture timeout --preserve-status -s TERM 1s "$@"
  if no_config_error "${STDOUT}${STDERR}"; then
    pass "$desc"
  else
    fail "$desc" "configuration/validation error surfaced"
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
  if no_config_error "$hay" && [[ "$hay" == *"$needle"* ]]; then
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
  local path="$1" out="$2" duration="${3:-4}"
  rm -f "$path"
  python3 - "$path" "$duration" >"$out" 2>"$out.err" <<'PY' &
import socket, sys, time, os
path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
try:
    sock.bind(path)
    sock.settimeout(0.2)
    end = time.time() + float(sys.argv[2])
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
  if [[ -n "${NOTIFY_RECV_PID:-}" ]]; then
    kill "$NOTIFY_RECV_PID" 2>/dev/null || true
    wait "$NOTIFY_RECV_PID" 2>/dev/null || true
  fi
  NOTIFY_RECV_PID=""
}

# Runs one NOTIFY_SOCKET scenario: start the receiver, spawn the binary with
# NOTIFY_SOCKET set, stop it with SIGTERM, then collect the datagrams in
# `messages` and the combined output in `hay`. Returns 1 when the receiver could
# not bind, so the caller can skip the case.
run_notify_case() {
  local wait_secs="$1" sock="$2" out="$3"; shift 3
  if ! start_notify_receiver "$sock" "$out" || [[ ! -S "$sock" ]]; then
    stop_notify_receiver
    rm -f "$sock" "$out" "$out.err"
    return 1
  fi
  spawn env NOTIFY_SOCKET="$sock" "$@"
  sleep "$wait_secs"
  kill -TERM "$PID" 2>/dev/null || true
  await 3
  stop_notify_receiver
  hay="${STDOUT}${STDERR}"
  # shellcheck disable=SC2034 # read by notify.sh
  messages="$(cat "$out" 2>/dev/null || true)"
  rm -f "$sock" "$out" "$out.err"
  return 0
}

# Runner resolves GETCAP; an empty value means capability detection is unavailable.
caps_str() { [[ -n "${GETCAP:-}" ]] && "$GETCAP" "$BIN" 2>/dev/null || true; }
