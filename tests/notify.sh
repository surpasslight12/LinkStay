#!/usr/bin/env bash
# NOTIFY_SOCKET protocol tests. Sourced by live.sh after the CAP_NET_RAW
# gate; uses helpers from lib.sh.
# shellcheck disable=SC2154 # hay/messages are set by lib.sh helpers
# 5) NOTIFY_SOCKET protocol: READY, STATUS, WATCHDOG, STOPPING
if command -v python3 >/dev/null 2>&1; then
  notify_sock="$TMP/notify.sock"
  notify_out="$TMP/notify.out"
  if run_notify_case 1.8 "$notify_sock" "$notify_out" \
       WATCHDOG_USEC=1000000 "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug; then
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
  else
    skip "NOTIFY_SOCKET protocol" "python3 receiver failed to bind $notify_sock"
  fi
else
  skip "NOTIFY_SOCKET protocol" "python3 not available"
fi

# 5b) Malformed WATCHDOG_USEC must disable the watchdog instead of arming a
#     deadline hundreds of millions of years away (a leading '-' wraps).
if command -v python3 >/dev/null 2>&1; then
  notify_sock="$TMP/notify-badusec.sock"
  notify_out="$TMP/notify-badusec.out"
  if run_notify_case 1.2 "$notify_sock" "$notify_out" \
       WATCHDOG_USEC=-1 "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug; then
    [[ "$hay" == *"watchdog disabled"* ]] && \
      pass "malformed WATCHDOG_USEC disables the watchdog" || \
      fail "malformed WATCHDOG_USEC disables the watchdog" "hay tail=${hay: -300}"
    [[ "$messages" != *"WATCHDOG=1"* ]] && \
      pass "no WATCHDOG beat with a malformed WATCHDOG_USEC" || \
      fail "no WATCHDOG beat with a malformed WATCHDOG_USEC" "messages=$messages"
  else
    skip "malformed WATCHDOG_USEC" "python3 receiver failed to bind"
  fi
else
  skip "malformed WATCHDOG_USEC" "python3 not available"
fi

# 6) WATCHDOG_PID mismatch disables watchdog while READY/STOPPING still work
if command -v python3 >/dev/null 2>&1; then
  notify_sock="$TMP/notify-pid.sock"
  notify_out="$TMP/notify-pid.out"
  if run_notify_case 1.2 "$notify_sock" "$notify_out" \
       WATCHDOG_USEC=1000000 WATCHDOG_PID=99999999 \
       "$BIN" -t 127.0.0.1 -i 1 -w 500 -l debug; then
    [[ "$hay" == *"watchdog disabled"* ]] && \
      pass "WATCHDOG_PID mismatch disables watchdog" || \
      fail "WATCHDOG_PID mismatch disables watchdog" "hay tail=${hay: -300}"
    [[ "$messages" == *"READY=1"* ]] && pass "notify READY works with WATCHDOG_PID set" || \
      fail "notify READY works with WATCHDOG_PID set" "messages=$messages"
    [[ "$messages" != *"WATCHDOG=1"* ]] && pass "no WATCHDOG with mismatched WATCHDOG_PID" || \
      fail "no WATCHDOG with mismatched WATCHDOG_PID" "messages=$messages"
    [[ "$messages" == *"STOPPING=1"* ]] && pass "notify STOPPING works with WATCHDOG_PID set" || \
      fail "notify STOPPING works with WATCHDOG_PID set" "messages=$messages"
  else
    skip "WATCHDOG_PID mismatch notify" "python3 receiver failed to bind $notify_sock"
  fi
else
  skip "WATCHDOG_PID mismatch notify" "python3 not available"
fi
