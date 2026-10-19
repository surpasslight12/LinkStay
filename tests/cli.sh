#!/usr/bin/env bash
# CLI, environment, validation, precedence, and debug-dump tests.
# Sourced by run_tests.sh; uses helpers from lib.sh.
section "Help & version"
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

section "Target IP literal validation"
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

section "Numeric arg validation"
expect_reject "--interval 0 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 0s
expect_reject "--interval -1 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i -1
expect_reject "--interval abc rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i abc
expect_reject "--threshold 0 rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 -n 0
expect_reject "--threshold xyz rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 --threshold xyz
expect_reject "--timeout 0 rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -w 0ms
expect_reject "--timeout negative rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -w -100
expect_accept "--interval 1s / --threshold 1 / --timeout 500ms accepted" \
  "$BIN" -t 1.1.1.1 -i 1s -n 1 -w 500ms

# Attached short arguments for required numeric options
expect_accept "attached -i2s" "$BIN" -t 1.1.1.1 -i2s -w 500ms -sfalse -l silent
expect_accept "attached -n3" "$BIN" -t 1.1.1.1 -n3 -w 500ms -sfalse -l silent
expect_accept "attached -w500ms" "$BIN" -t 1.1.1.1 -w500ms -sfalse -l silent
expect_accept "attached -ldebug" "$BIN" -t 1.1.1.1 -w500ms -ldebug -sfalse

# Empty and junk values
expect_reject "--interval= empty" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 --interval=
expect_reject "--threshold= empty" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 --threshold=
expect_reject "--timeout= empty" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 --timeout=
expect_reject "--interval with trailing junk" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 5x -w 500ms
expect_reject "--timeout overflow" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 1s -w 999999999999999999999999ms

# Integer boundaries: parser accepts INT_MAX and rejects INT_MAX+1
expect_accept "--interval INT_MAX accepted" \
  "$BIN" -t 1.1.1.1 -i 2147483647s -sfalse -l silent
expect_reject "--interval INT_MAX+1 rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i 2147483648s
expect_accept "--threshold INT_MAX accepted" \
  "$BIN" -t 1.1.1.1 -n 2147483647 -sfalse -l silent
expect_reject "--threshold INT_MAX+1 rejected" "Invalid value for --threshold" \
  "$BIN" -t 1.1.1.1 -n 2147483648
expect_accept "--timeout INT_MAX accepted with matching interval" \
  "$BIN" -t 1.1.1.1 -i 2147484s -w 2147483647ms -sfalse -l silent
expect_reject "--timeout INT_MAX+1 rejected" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 2147484s -w 2147483648ms

# Leading zeros stay accepted; a leading '+' is a legacy spelling and is
# rejected, as are leading whitespace and '-'.
expect_reject "leading + rejected" "Invalid value for --interval" \
  "$BIN" -t 1.1.1.1 -i +5s -w 1000ms -sfalse -l silent
expect_accept "leading zeros accepted" \
  "$BIN" -t 1.1.1.1 -i 0005s -w 1000ms -sfalse -l silent

section "Duration suffix parsing (s / ms)"

# Duration values require an explicit suffix; bare -i 2 is rejected.
expect_reject "bare -i 2 rejected (suffix required)" \
  "Invalid value for --interval" "$BIN" -t 1.1.1.1 -i 2 -w 500ms -sfalse -l silent
expect_accept "-i 500ms accepted" "$BIN" -t 1.1.1.1 -i 500ms -w 400ms -sfalse -l silent
expect_accept "-i 2s accepted" "$BIN" -t 1.1.1.1 -i 2s -w 500ms -sfalse -l silent
expect_accept "-i 0005ms accepted (leading zeros)" \
  "$BIN" -t 1.1.1.1 -i 0005ms -w 400ms -sfalse -l silent
expect_accept "-w 1s accepted" "$BIN" -t 1.1.1.1 -i 2s -w 1s -sfalse -l silent
expect_accept "-w 500ms accepted" "$BIN" -t 1.1.1.1 -i 1s -w 500ms -sfalse -l silent
expect_reject "-i 5S rejected (lowercase suffixes only)" \
  "Invalid value for --interval" "$BIN" -t 1.1.1.1 -i 5S -w 500ms
expect_reject "-w 1.5s rejected (integers only)" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 2s -w 1.5s
expect_reject "-w 1m rejected (unknown suffix)" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 2s -w 1m
expect_reject "interval in ms must still exceed timeout" \
  "must be smaller than interval" "$BIN" -t 1.1.1.1 -i 500ms -w 600ms
expect_reject "-w 2147484s exceeds INT_MAX ms" "Invalid value for --timeout" \
  "$BIN" -t 1.1.1.1 -i 2147485s -w 2147484s
expect_debug_contains "dump shows ms-suffixed interval" "Interval: 1500ms" \
  "$BIN" -t 1.1.1.1 -i 1500ms -w 500ms -l debug -sfalse
expect_debug_contains "dump shows s-suffixed timeout" "Timeout: 1s" \
  "$BIN" -t 1.1.1.1 -i 2s -w 1s -l debug -sfalse
expect_accept "LINKSTAY_INTERVAL with ms suffix" \
  env LINKSTAY_INTERVAL=500ms "$BIN" -t 1.1.1.1 -w 400ms -sfalse -l silent
expect_reject "LINKSTAY_TIMEOUT with unknown suffix" \
  "Invalid value for LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=1m "$BIN" -t 1.1.1.1

section "Cross-field validation"
expect_reject "timeout > interval rejected" "must be smaller than interval" \
  "$BIN" -t 1.1.1.1 -i 1s -w 5000ms
expect_reject "timeout == interval rejected" "must be smaller than interval" \
  "$BIN" -t 1.1.1.1 -i 1s -w 1000ms
expect_accept "timeout < interval accepted" "$BIN" -t 1.1.1.1 -i 2s -w 1500ms
expect_accept "timeout just below interval accepted (no hidden margin)" \
  "$BIN" -t 1.1.1.1 -i 1s -w 999ms

section "Poweroff parsing"

# Boolean values are limited to true/false (case-insensitive); every other
# historical spelling must be rejected with a message naming the accepted set.
expect_accept "--poweroff=false" "$BIN" -t 1.1.1.1 --poweroff=false
expect_accept "-pfalse (attached)" "$BIN" -t 1.1.1.1 -pfalse
expect_accept "-pFALSE (case-insensitive)" "$BIN" -t 1.1.1.1 -pFALSE
expect_accept "--poweroff omitted" "$BIN" -t 1.1.1.1
expect_reject "--poweroff invalid" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 --poweroff=bogus
expect_reject "--poweroff= empty" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 --poweroff=
expect_reject "--poweroff=1 rejected (only true/false)" \
  "Invalid value for --poweroff" "$BIN" -t 1.1.1.1 --poweroff=1
expect_reject "-p0 rejected (only true/false)" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 -p0
expect_reject "-pNO rejected (only true/false)" "Invalid value for --poweroff" \
  "$BIN" -t 1.1.1.1 -pNO
expect_reject "--poweroff=yes rejected (only true/false)" \
  "Invalid value for --poweroff" "$BIN" -t 1.1.1.1 --poweroff=yes
expect_accept "--poweroff false accepted as separated value" \
  "$BIN" -t 1.1.1.1 --poweroff false -sfalse

# --poweroff=true is validated against the systemd runtime.
if (( SYSTEMD_RUNTIME )); then
  expect_accept "--poweroff=true accepted on systemd host" \
    "$BIN" -t 1.1.1.1 --poweroff=true
else
  expect_reject "--poweroff=true rejected on non-systemd host" \
    "poweroff requires a systemd host" \
    "$BIN" -t 1.1.1.1 --poweroff=true
fi

# Bare boolean flags are no longer accepted; an explicit value is required.
expect_reject "bare -p rejected (value required)" "Option requires an argument" \
  "$BIN" -t 127.0.0.1 -p
expect_reject "bare --poweroff rejected (value required)" \
  "Option requires an argument" "$BIN" -t 127.0.0.1 --poweroff

section "Log-level parsing"
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
run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l silent -sfalse
hay="${STDOUT}${STDERR}"
[[ -z "$hay" ]] && pass "silent suppresses even init errors" || \
  fail "silent suppresses even init errors" "hay=$hay"

section "Bool / systemd flag parsing"
for v in true false; do
  expect_accept "--systemd=$v" "$BIN" -t 1.1.1.1 --systemd="$v"
done
expect_accept "--systemd=TRUE case-insensitive" \
  "$BIN" -t 1.1.1.1 --systemd=TRUE
expect_reject "bare --systemd rejected (value required)" "Option requires an argument" \
  "$BIN" -t 1.1.1.1 --systemd
expect_accept "-sfalse disables systemd" "$BIN" -t 1.1.1.1 -sfalse
expect_accept "-sFALSE case-insensitive" "$BIN" -t 1.1.1.1 -sFALSE
expect_reject "bare -s rejected (value required)" "Option requires an argument" \
  "$BIN" -t 1.1.1.1 -s
expect_reject "--systemd=bogus rejected" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 --systemd=bogus
expect_reject "--systemd= empty rejected" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 --systemd=
expect_reject "--systemd=0 rejected (only true/false)" \
  "Invalid value for --systemd" "$BIN" -t 1.1.1.1 --systemd=0
expect_reject "-s0 rejected (only true/false)" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 -s0
expect_reject "-sYES rejected (only true/false)" "Invalid value for --systemd" \
  "$BIN" -t 1.1.1.1 -sYES
expect_accept "--systemd false accepted as separated value" \
  "$BIN" -t 1.1.1.1 --systemd false

section "CLI parse errors"
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

section "Environment variables"

# Each env var is accepted on its own
expect_accept "LINKSTAY_TARGET accepted" \
  env LINKSTAY_TARGET=8.8.8.8 "$BIN" -l silent -sfalse
expect_accept "LINKSTAY_INTERVAL accepted" \
  env LINKSTAY_INTERVAL=5s "$BIN" -t 1.1.1.1 -w 400ms -sfalse -l silent
expect_accept "LINKSTAY_THRESHOLD accepted" \
  env LINKSTAY_THRESHOLD=4 "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_TIMEOUT accepted" \
  env LINKSTAY_TIMEOUT=400ms "$BIN" -t 1.1.1.1 -sfalse -l silent
expect_accept "LINKSTAY_POWEROFF=false accepted" \
  env LINKSTAY_POWEROFF=false "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_LOG_LEVEL=debug accepted" \
  env LINKSTAY_LOG_LEVEL=debug "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_SYSTEMD=false accepted" \
  env LINKSTAY_SYSTEMD=false "$BIN" -t 1.1.1.1

# Bool env vars accept only true/false, case-insensitively
expect_accept "LINKSTAY_SYSTEMD=true accepted" \
  env LINKSTAY_SYSTEMD=true "$BIN" -t 1.1.1.1
expect_accept "LINKSTAY_SYSTEMD=FALSE case-insensitive" \
  env LINKSTAY_SYSTEMD=FALSE "$BIN" -t 1.1.1.1
if (( SYSTEMD_RUNTIME )); then
  expect_accept "LINKSTAY_POWEROFF=true accepted on systemd host" \
    env LINKSTAY_POWEROFF=true "$BIN" -t 1.1.1.1
else
  expect_reject "LINKSTAY_POWEROFF=true rejected on non-systemd host" \
    "poweroff requires a systemd host" \
    env LINKSTAY_POWEROFF=true "$BIN" -t 1.1.1.1
fi
for v in 1 0 yes no on off; do
  expect_reject "LINKSTAY_SYSTEMD=$v rejected (only true/false)" \
    "Invalid value for LINKSTAY_SYSTEMD" \
    env LINKSTAY_SYSTEMD="$v" "$BIN" -t 1.1.1.1
done
expect_reject "LINKSTAY_POWEROFF=0 rejected (only true/false)" \
  "Invalid value for LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=0 "$BIN" -t 1.1.1.1

# Invalid env values are rejected with the env var name
for pair in LINKSTAY_INTERVAL=abc LINKSTAY_THRESHOLD=abc LINKSTAY_TIMEOUT=0ms \
            LINKSTAY_POWEROFF=junk LINKSTAY_LOG_LEVEL=junk \
            LINKSTAY_SYSTEMD=junk; do
  var="${pair%%=*}"
  expect_reject "$pair rejected" "Invalid value for $var" \
    env "$pair" "$BIN" -t 1.1.1.1
done
for var in LINKSTAY_INTERVAL LINKSTAY_THRESHOLD LINKSTAY_TIMEOUT \
           LINKSTAY_POWEROFF LINKSTAY_LOG_LEVEL LINKSTAY_SYSTEMD; do
  expect_reject "$var= empty rejected" "Invalid value for $var" \
    env "$var=" "$BIN" -t 1.1.1.1
done

section "Env / CLI precedence"

# CLI must override env: env invalid target overridden by valid CLI target
scrub_env
LINKSTAY_TARGET=invalid run_capture timeout --preserve-status -s TERM 1s "$BIN" -t 1.1.1.1 -l silent -sfalse
[[ "$STDERR" != *"Invalid IPv4/IPv6"* && "$STDERR" != *"Target must be a valid"* ]] && \
  pass "CLI --target overrides invalid LINKSTAY_TARGET" || \
  fail "CLI --target overrides invalid LINKSTAY_TARGET" "$STDERR"

# Env invalid value still rejected when CLI does not override it
expect_reject "env-invalid surfaces when CLI does not override" \
  "Invalid value for LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=foo "$BIN" -t 1.1.1.1

# CLI must override even an invalid env value, not just an invalid string value
expect_accept "CLI --interval overrides invalid LINKSTAY_INTERVAL" \
  env LINKSTAY_INTERVAL=foo "$BIN" -t 1.1.1.1 -i 2s -w 500ms
expect_accept "CLI --threshold overrides invalid LINKSTAY_THRESHOLD" \
  env LINKSTAY_THRESHOLD=foo "$BIN" -t 1.1.1.1 -n 3 -w 500ms
expect_accept "CLI --timeout overrides invalid LINKSTAY_TIMEOUT" \
  env LINKSTAY_TIMEOUT=foo "$BIN" -t 1.1.1.1 -w 500ms
expect_accept "CLI --poweroff overrides invalid LINKSTAY_POWEROFF" \
  env LINKSTAY_POWEROFF=junk "$BIN" -t 1.1.1.1 -pfalse -w 500ms
expect_accept "CLI --log-level overrides invalid LINKSTAY_LOG_LEVEL" \
  env LINKSTAY_LOG_LEVEL=junk "$BIN" -t 1.1.1.1 -l debug -w 500ms -sfalse
expect_accept "CLI --systemd overrides invalid LINKSTAY_SYSTEMD" \
  env LINKSTAY_SYSTEMD=junk "$BIN" -t 1.1.1.1 -sfalse -w 500ms

section "Debug config dump"

# The debug dump is emitted before the raw socket is opened, so it can be
# checked without CAP_NET_RAW.
expect_debug_contains "dump shows target" "Target: 8.8.8.8" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "dump shows interval" "Interval: 7s" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "dump shows threshold" "Threshold: 4" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "dump shows timeout" "Timeout: 600ms" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "dump shows poweroff false" "Poweroff: false" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "dump shows log level" "Log Level: DEBUG" \
  "$BIN" -t 8.8.8.8 -i 7s -w 600ms -n 4 -pfalse -l debug -sfalse
expect_debug_contains "systemd=false enables timestamps" "Timestamp: true" \
  "$BIN" -t 8.8.8.8 -l debug -sfalse
expect_debug_contains "systemd=true disables timestamps" "Timestamp: false" \
  "$BIN" -t 8.8.8.8 -l debug --systemd=true

# CLI values win over valid env values for every option; the debug dump lets
# us observe the final resolved configuration.
expect_debug_contains "CLI target wins over valid env target" "Target: 1.1.1.1" \
  env LINKSTAY_TARGET=8.8.8.8 \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI interval wins over valid env interval" "Interval: 2s" \
  env LINKSTAY_INTERVAL=9s \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI timeout wins over valid env timeout" "Timeout: 500ms" \
  env LINKSTAY_TIMEOUT=900ms \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI threshold wins over valid env threshold" "Threshold: 3" \
  env LINKSTAY_THRESHOLD=9 \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI poweroff wins over valid env poweroff" "Poweroff: false" \
  env LINKSTAY_POWEROFF=true \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI log-level wins over valid env log-level" "Log Level: DEBUG" \
  env LINKSTAY_LOG_LEVEL=error \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
expect_debug_contains "CLI systemd wins over valid env systemd" "Systemd: false" \
  env LINKSTAY_SYSTEMD=true \
  "$BIN" -t 1.1.1.1 -i 2s -w 500ms -n 3 -pfalse -l debug -sfalse
