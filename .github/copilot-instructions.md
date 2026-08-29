# linkstay Copilot Instructions

## Build and lint commands

- Build the binary with `make`. The output is `bin/linkstay`.
- Build a stripped release binary with `make release`.
- Run linters with `make lint` (`cppcheck` + `clang-tidy`). At the current baseline this command completes cleanly; keep it that way in your changes.
- Source layout is flat: all headers (`.h`) and translation units (`.c`) live together in `src/`. The Makefile adds `-Isrc` so quoted includes such as `#include "app.h"` resolve without relative paths.
- The repository intentionally has no `LICENSE` file or license badge. Do not re-add one unless the user explicitly asks.

## High-level architecture

- `src/main.c` is a thin entry point: `ls_opts_resolve()` parses defaults, environment, and CLI; `ls_app_init()` wires runtime state; `ls_app_run()` registers callbacks into the event loop and runs it.
- `src/opts.c` uses a static `getopt_long` option table plus direct per-option parsers. Precedence is defaults → CLI → environment, with a `cli_seen[]` bitmap so an env var is only applied when the corresponding CLI option was not given; invalid env values therefore do not defeat valid CLI overrides. Duration options (`--interval`, `--timeout`) parse via `apply_duration()`: a plain number keeps its historical unit (interval = seconds, timeout = milliseconds), an optional lowercase `s`/`ms` suffix overrides it, and the result is always stored as milliseconds in `ls_opts_t` (`interval_ms`, `timeout_ms`). Cross-field validation (IP literal, timeout < interval, systemd runtime check for poweroff) is an explicit post step. `--help`/`--version` use a getopt pre-scan so they win over invalid env/args.
- `src/loop.c` is a generic single-threaded event loop: fixed-capacity fd slots (pollfd + callback) and timer slots (absolute deadline in monotonic ms, `UINT64_MAX` = disarmed, phase-preserving `ls_timer_step`), plus an integrated signalfd channel. Timer contract: a fired callback MUST re-arm or disarm its timer. No dynamic registration removal, no vtables.
- `src/app.c` is the application assembly: it owns the explicit probe FSM (`LS_PROBE_IDLE` ↔ `LS_PROBE_AWAIT_REPLY`), the consecutive-failure counter, threshold → action dispatch, statistics logging (SIGUSR1 + shutdown), and the startup/shutdown banners. It registers three timers (watchdog, reply deadline, ping scheduler — in that firing order) and the ICMP fd + signal callbacks into the loop. Statistics, threshold action (`posix_spawn` + 1s startup observation), and `sd_notify`-style integration (READY/STATUS with 2s dedup/WATCHDOG/STOPPING) are private `static` implementation sections of `app.c`; only their state types are exposed in `app.h` because `ls_app_t` embeds them. The notify socket is non-blocking. `systemd/linkstay.service` expects this with `Type=notify`, `NotifyAccess=main`, and `WatchdogSec=30`. Notifier helpers are no-ops when `enabled=false`; `ls_app_init` sets `notify.sockfd = -1` up front so destroy never closes fd 0.
- `src/icmp.c` owns raw-socket ICMP send/receive logic, packet construction, reply matching, and optional BPF socket filters for IPv4/IPv6 traffic (non-fatal when unavailable). IPv4 raw sockets deliver the IP header (skip `ip_hl*4`); IPv6 raw sockets deliver payload only (kernel strips the header) — both are correct, don't "fix" them.
- `src/base.c` + `src/base.h` form the dependency-free foundation: `ls_now_ms()`, `ls_add_sat()`, the unified error type `ls_err_t` (`char msg[256]`), and five-level logging (`ls_error/ls_warn` are `[[gnu::cold]]`, `ls_info/ls_debug` inline-gated). Every fallible init/parse/resolve function returns `bool` + `ls_err_t *` out-param via `ls_err_set()` — this is the single error-reporting convention; do not add ad-hoc `(char *buf, size_t size)` pairs.
- All public symbols use the `ls_` prefix; macros use `LS_`. Env vars remain `LINKSTAY_*`.
- Top-level structure is intentionally small: `src/` for all source and headers, `systemd/` for deployable unit examples, `.github/copilot-instructions.md` for AI collaboration rules, and `Makefile` for local build entry points.

## CLI options and environment variables

All short options are lowercase and mnemonic:

| Short | Long | Env var | Description |
|-------|------|---------|-------------|
| `-t` | `--target` | `LINKSTAY_TARGET` | IP literal target (no DNS) |
| `-i` | `--interval` | `LINKSTAY_INTERVAL` | Probe interval; plain numbers mean seconds, `s`/`ms` suffixes accepted |
| `-n` | `--threshold` | `LINKSTAY_THRESHOLD` | Consecutive failure threshold |
| `-w` | `--timeout` | `LINKSTAY_TIMEOUT` | Reply timeout; plain numbers mean milliseconds, `s`/`ms` suffixes accepted |
| `-p` | `--poweroff` | `LINKSTAY_POWEROFF` | Optional bool; bare flag = true; when true powers off via systemctl, when false only simulates (dry-run). Accepts true/false/1/0/yes/no/on/off |
| `-l` | `--log-level` | `LINKSTAY_LOG_LEVEL` | Log level: silent/error/warn/info/debug |
| `-s` | `--systemd` | `LINKSTAY_SYSTEMD` | Optional bool; bare flag = true; accepts true/false/1/0/yes/no/on/off |
| `-v` | `--version` | — | Show version |
| `-h` | `--help` | — | Show help |

Effective precedence: defaults → environment variables → CLI arguments. The parser applies CLI first and consults an env var only when the corresponding CLI option was not given. `ls_opts_resolve()` is the single orchestration path.

## Key conventions

- `LINKSTAY_TARGET` and `--target` accept only IPv4/IPv6 literals. DNS names are intentionally rejected.
- `--systemd` / `-s` uses an optional boolean argument. A bare flag enables integration; explicit disable uses `--systemd=0`, `--systemd=false`, `-s0`, or `-sfalse`.
- `poweroff=false` (the default) is a terminal simulation path: once the threshold is reached and the simulated shutdown path completes, the process exits. Set `poweroff=true` to perform a real `systemctl poweroff`.
- Log timestamps are derived from systemd integration state, not a separate config knob: timestamps are disabled when systemd logging is enabled and enabled otherwise.
- The codebase prefers stack buffers over heap allocation — zero dynamic allocation anywhere. Fallible functions return `bool` + `ls_err_t *` (see base.h); status-shaped results use small enums (`ls_icmp_recv_status_t`, `ls_action_result_t`).
- `restrict` is reserved for public API declarations (headers and the definitions that implement them); translation-unit `static` helpers never use it.
- Comment style is uniform: file headers open with a lone `/*` line, continue with ` * name — one-line description.`, and close with a lone ` */`; section markers are single-line `/* ---- Section ---- */`; other multi-line comments open on the `/*` line and close inline on the last text line. Do not mix `=`-rule separators or `*/`-alone closers.
- Adjacent string literals concatenate directly (implicit concatenation); never use a trailing `\` line-continuation inside string literals. The only allowed `\` continuations are inside `#define`.
- Prefer `sizeof(*ptr)` over `sizeof(struct X)` whenever a pointer to the type is already in scope.
- Time handling: every duration is millisecond-based internally. `ls_opts_t` stores `interval_ms`/`timeout_ms` as `uint64_t`; `base.h` owns the integer unit constants (`LS_NS_PER_MS`, `LS_US_PER_MS`, `LS_MS_PER_SEC`) and `ls_format_duration()` (whole seconds render as `10s`, otherwise `1500ms`). User-facing durations (`--interval`/`--timeout` and their env vars) accept an optional lowercase `s`/`ms` suffix via `apply_duration()`; use it for any new duration option. Do not introduce ad-hoc `* 1000` conversions or raw `50000000L`-style constants.
- Headers form a layered hierarchy rooted at `src/base.h` (no linkstay dependencies) and live alongside their implementations in `src/`. Each module owns a focused header; put shared enums, constants, structs, and public declarations in the header of the module that owns them, and keep orchestration helpers `static` inside `app.c` unless another translation unit genuinely needs them.
- The monitoring logic is timer/state-machine driven. `loop.c` owns the generic timer slots (absolute deadline word, UINT64_MAX = disarmed, inline `ls_timer_disarm`/`ls_timer_arm_after`/`ls_timer_step` helpers); `app.c` owns the probe FSM and arms/steps the timers from its callbacks. No sleeping loops, no threads; avoid reintroducing per-action wrapper functions.
- systemd integration lives inside `app.c` behind `ls_notify_t`. There is no runtime-services vtable: the app calls `ls_notify_*` directly and the notifier no-ops when not enabled. If another integration is needed, add a sibling notifier module and a small dispatcher in `app.c` — do not resurrect a generic vtable for a single backend.
