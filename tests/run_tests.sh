#!/usr/bin/env bash
# shellcheck disable=SC2034 # GETCAP/SYSTEMD_RUNTIME are consumed by sourced files
# Comprehensive functional test runner for the linkstay systemd service.
# - config.sh covers environment parsing, validation, and error messages.
# - live.sh covers live ICMP, signals, and the threshold action.
# - notify.sh (sourced by live.sh) covers the NOTIFY_SOCKET protocol.
# - tests/unit.c is built and run by `make test` before this script.
# Exits non-zero if any test failed.
#
# Environment knobs:
#   LINKSTAY_TEST_LIVE=1
#     Force the live ICMP suite even when `getcap` reports no cap_net_raw. Use
#     it inside a user+network namespace, where the binary is root and owns the
#     network namespace (containers cannot set file capabilities):
#       unshare -Urn --map-root-user bash -c \
#         'ip link set lo up; LINKSTAY_TEST_LIVE=1 make test'
#   LINKSTAY_TEST_UNREACHABLE_TARGET=<ip>
#     Target used by the "no reply reaches the threshold" test (default
#     192.0.2.1, RFC 5737 TEST-NET-1). A bare network namespace has no default
#     route, so point this at an unroutable address to exercise the
#     ENETUNREACH send-failure path instead of a silent timeout.

set -u

TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$TEST_DIR/.." && pwd)"
BIN="${BIN:-$ROOT/bin/linkstay}"
TMP="$(mktemp -d)"
GLOBAL_NOTIFY_PID=""

cleanup() {
  stop_notify_receiver
  if [[ -n "$GLOBAL_NOTIFY_PID" ]]; then
    kill "$GLOBAL_NOTIFY_PID" 2>/dev/null || true
    wait "$GLOBAL_NOTIFY_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

# shellcheck source=lib.sh
. "$TEST_DIR/lib.sh"

if [[ ! -x "$BIN" ]]; then
  echo "Error: binary not found at $BIN. Run 'make' first." >&2
  exit 2
fi

# getcap may live in /sbin, which is not always in PATH
GETCAP=""
for p in getcap /sbin/getcap /usr/sbin/getcap; do
  if command -v "$p" >/dev/null 2>&1; then GETCAP="$p"; break; fi
done

SYSTEMD_RUNTIME=0
if [[ -x /usr/bin/systemctl && -d /run/systemd/system ]]; then
  SYSTEMD_RUNTIME=1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 is required for the NOTIFY_SOCKET test receiver" >&2
  exit 2
fi

# Every service invocation needs a notify channel. Start one receiver for the
# whole suite; protocol tests below start their own receiver and override
# NOTIFY_SOCKET explicitly.
TEST_NOTIFY_SOCKET="$TMP/notify-global.sock"
if ! start_notify_receiver "$TEST_NOTIFY_SOCKET" "$TMP/notify-global.out" 300 \
     || [[ ! -S "$TEST_NOTIFY_SOCKET" ]]; then
  echo "Error: could not start the NOTIFY_SOCKET test receiver" >&2
  exit 2
fi
GLOBAL_NOTIFY_PID="$NOTIFY_RECV_PID"
export TEST_NOTIFY_SOCKET

printf "Test target: %s\n" "$BIN"
printf "Capabilities: %s\n" "$(caps_str || echo none)"

# shellcheck source=config.sh
. "$TEST_DIR/config.sh"

# shellcheck source=live.sh
. "$TEST_DIR/live.sh"

section "Summary"

printf "Pass: %d   Fail: %d   Skip: %d\n" "$PASS" "$FAIL" "$SKIP"
if (( FAIL > 0 )); then
  printf "\n%sFailed tests:%s\n" "$R" "$X"
  for t in "${FAILED[@]}"; do printf "  - %s\n" "$t"; done
  exit 1
fi
exit 0
