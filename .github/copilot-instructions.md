# linkstay Copilot Instructions

## Build and lint commands

- Build the binary with `make`. The output is `bin/linkstay`.
- Build a stripped release binary with `make release`.
- Run linters with `make lint` (`cppcheck` + `clang-tidy`). At the current baseline this command completes but reports existing findings, so do not assume the repository is lint-clean before your changes.
- Source layout is split: all public headers live in `include/` and all translation units (`.c`) live in `src/`. The Makefile adds `-Iinclude` so quoted includes such as `#include "app.h"` resolve without relative paths.
- The repository intentionally has no `LICENSE` file or license badge. Do not re-add one unless the user explicitly asks.

## High-level architecture

- `src/main.c` is a thin entry point: `ls_opts_resolve()` parses defaults, environment, and CLI; `ls_app_init()` wires runtime state; `ls_app_run()` registers callbacks into the event loop and runs it.
- `src/opts.c` is a table-driven configuration engine: a single `OPTION_TABLE` of descriptors (short/long option, env var, kind, `offsetof` target) drives env loading, getopt optstring/longopts construction, and value application. Cross-field validation (IP literal, timeout < interval, systemd runtime check for poweroff) is an explicit post step. `--help`/`--version` use a getopt pre-scan so they win over invalid env/args.
- `src/loop.c` is a generic single-threaded event loop: fixed-capacity fd slots (pollfd + callback) and timer slots (absolute deadline in monotonic ms, `UINT64_MAX` = disarmed, phase-preserving `ls_timer_step`), plus an integrated signalfd channel. Timer contract: a fired callback MUST re-arm or disarm its timer. No dynamic registration removal, no vtables.
- `src/app.c` is the application assembly: it owns the explicit probe FSM (`LS_PROBE_IDLE` ↔ `LS_PROBE_AWAIT_REPLY`), the consecutive-failure counter, threshold → action dispatch, statistics logging (SIGUSR1 + shutdown), and the startup/shutdown banners. It registers three timers (watchdog, reply deadline, ping scheduler — in that firing order) and the ICMP fd + signal callbacks into the loop.
- `src/icmp.c` owns raw-socket ICMP send/receive logic, packet construction, reply matching, and optional BPF socket filters for IPv4/IPv6 traffic (non-fatal when unavailable). IPv4 raw sockets deliver the IP header (skip `ip_hl*4`); IPv6 raw sockets deliver payload only (kernel strips the header) — both are correct, don't "fix" them.
- `src/action.c` is the threshold action backend. When `poweroff=true` it invokes `systemctl --no-block poweroff` via `posix_spawn()` plus a 1s startup observation; when `poweroff=false` it only logs a dry-run message (`LS_ACTION_SIMULATED`).
- `src/notify.c` implements `sd_notify`-style integration directly over the notify socket (READY/STATUS with 2s dedup/WATCHDOG/STOPPING). `systemd/linkstay.service` expects this with `Type=notify`, `NotifyAccess=main`, and `WatchdogSec=30`; the shipped sample unit defaults to `LINKSTAY_POWEROFF=false`. Notifier helpers are no-ops when `enabled=false`, so callers never guard; `ls_app_init` sets `notify.sockfd = -1` up front so destroy never closes fd 0.
- `src/stats.c` aggregates ping statistics (counts, latency extremes, uptime) behind a small value type.
- `src/log.c` + `include/log.h` provide five-level logging (`ls_error/ls_warn` are `[[gnu::cold]]`, `ls_info/ls_debug` inline-gated).
- `src/base.c` + `include/base.h` form the dependency-free foundation: `ls_now_ms()`, `ls_add_sat()`, and the unified error type `ls_err_t` (`char msg[256]`). Every fallible init/parse/resolve function returns `bool` + `ls_err_t *` out-param via `ls_err_set()` — this is the single error-reporting convention; do not add ad-hoc `(char *buf, size_t size)` pairs.
- All public symbols use the `ls_` prefix; macros use `LS_`. Env vars remain `LINKSTAY_*`.
- Top-level structure is intentionally small: `include/` for public module interfaces, `src/` for implementations, `systemd/` for deployable unit examples, `.github/copilot-instructions.md` for AI collaboration rules, and `Makefile` for local build entry points.

## CLI options and environment variables

All short options are lowercase and mnemonic:

| Short | Long | Env var | Description |
|-------|------|---------|-------------|
| `-t` | `--target` | `LINKSTAY_TARGET` | IP literal target (no DNS) |
| `-i` | `--interval` | `LINKSTAY_INTERVAL` | Ping interval (seconds) |
| `-n` | `--threshold` | `LINKSTAY_THRESHOLD` | Consecutive failure threshold |
| `-w` | `--timeout` | `LINKSTAY_TIMEOUT` | Ping timeout (milliseconds) |
| `-p` | `--poweroff` | `LINKSTAY_POWEROFF` | Optional bool; bare flag = true; when true powers off via systemctl, when false only simulates (dry-run). Accepts true/false/1/0/yes/no/on/off |
| `-l` | `--log-level` | `LINKSTAY_LOG_LEVEL` | Log level: silent/error/warn/info/debug |
| `-s` | `--systemd` | `LINKSTAY_SYSTEMD` | Optional bool; bare flag = true; accepts true/false/1/0/yes/no/on/off |
| `-v` | `--version` | — | Show version |
| `-h` | `--help` | — | Show help |

Config precedence: defaults → environment variables → CLI arguments. `ls_opts_resolve()` is the single orchestration path.

## Key conventions

- `LINKSTAY_TARGET` and `--target` accept only IPv4/IPv6 literals. DNS names are intentionally rejected.
- `--systemd` / `-s` uses an optional boolean argument. A bare flag enables integration; explicit disable uses `--systemd=0`, `--systemd=false`, `-s0`, or `-sfalse`.
- `poweroff=false` (the default) is a terminal simulation path: once the threshold is reached and the simulated shutdown path completes, the process exits. Set `poweroff=true` to perform a real `systemctl poweroff`.
- Log timestamps are derived from systemd integration state, not a separate config knob: timestamps are disabled when systemd logging is enabled and enabled otherwise.
- The codebase prefers stack buffers over heap allocation — zero dynamic allocation anywhere. Fallible functions return `bool` + `ls_err_t *` (see base.h); status-shaped results use small enums (`ls_icmp_recv_status_t`, `ls_action_result_t`).
- Headers form a layered hierarchy rooted at `include/base.h` (no linkstay dependencies) and live in `include/`, while implementations live in `src/`. Each module owns a focused header; put shared enums, constants, structs, and public declarations in the header of the module that owns them, and keep orchestration helpers `static` inside `app.c` unless another translation unit genuinely needs them.
- The monitoring logic is timer/state-machine driven. `loop.c` owns the generic timer slots (absolute deadline word, UINT64_MAX = disarmed, inline `ls_timer_disarm`/`ls_timer_arm_after`/`ls_timer_step` helpers); `app.c` owns the probe FSM and arms/steps the timers from its callbacks. No sleeping loops, no threads; avoid reintroducing per-action wrapper functions.
- systemd integration lives behind `ls_notify_t`. There is no runtime-services vtable: the app calls `ls_notify_*` directly and the notifier no-ops when not enabled. If another integration is needed, add a sibling notifier module and a small dispatcher in `app.c` — do not resurrect a generic vtable for a single backend.
