# LinkStay Copilot Instructions

## Build and lint commands

- Build the binary with `make`. The output is `bin/linkstay`.
- Build a stripped release binary with `make release`.
- Run linters with `make lint` (`cppcheck` + `clang-tidy`). At the current baseline this command completes but reports existing findings, so do not assume the repository is lint-clean before your changes.
- Source layout is split: all public headers live in `include/` and all translation units (`.c`) live in `src/`. The Makefile adds `-Iinclude` so quoted includes such as `#include "monitor.h"` resolve without relative paths.
- The repository intentionally has no `LICENSE` file or license badge. Do not re-add one unless the user explicitly asks.

## High-level architecture

- `src/main.c` is a thin entry point: `config_resolve()` parses defaults, environment, and CLI; `linkstay_ctx_init()` wires runtime state; `linkstay_reactor_run()` owns the monitor lifecycle.
- `src/config.c` is a single configuration module: defaults, environment parsing, getopt-based CLI parsing, `--help` / `--version`, validation, and config debug rendering live together.
- `src/monitor.c` is the core orchestration: the reactor loop, signal handling, ping scheduling, reply deadlines, shutdown FSM, and systemd watchdog scheduling. The loop uses `poll()` plus `signalfd`, not threaded workers. It calls `systemd_notifier_*` directly — there is no separate runtime-services indirection. Ping statistics live in `src/metrics.c`.
- `src/metrics.c` aggregates ping statistics (counts, latency extremes, uptime) behind a small value type.
- `src/icmp.c` owns raw-socket ICMP send/receive logic, packet construction, reply matching, and optional BPF socket filters for IPv4/IPv6 traffic.
- `src/shutdown.c` is the shutdown backend layer. When `poweroff=true` it invokes `systemctl poweroff`; when `poweroff=false` it only simulates (dry-run), and it uses `posix_spawn()` plus startup observation instead of shelling out.
- `src/systemd.c` implements `sd_notify`-style integration directly over the notify socket. `systemd/linkstay.service` expects this with `Type=notify`, `NotifyAccess=main`, and `WatchdogSec=30`; the shipped sample unit defaults to `LINKSTAY_POWEROFF=false` so the service only simulates until operators explicitly set `LINKSTAY_POWEROFF=true`. Notifier helpers are no-ops when `enabled=false`, so the monitor can call them unconditionally; the monitor sets `systemd.sockfd = -1` before optionally calling `systemd_notifier_init()` so cleanup never closes fd 0.
- `src/common.c` is intentionally minimal: it owns only `get_monotonic_ms()`. Use `<string.h>` (`memset`, `memcpy`) and `<stdio.h>` (`snprintf`, `vsnprintf`) directly — there are no custom mem/format wrappers.
- Headers are organized as a layered hierarchy under `include/`: `include/common.h` is the dependency-free foundation; each module owns its own header (`logger.h`, `metrics.h`, `config.h`, `icmp.h`, `shutdown.h`, `systemd.h`); `include/monitor.h` aggregates them and defines `linkstay_ctx_t`, the shared runtime object holding config, resolved destination address, logger, metrics, ICMP state, and the systemd notifier. The matching `.c` implementations live in `src/`.
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
| `-l` | `--log-level` | `LINKSTAY_LOG_LEVEL` | Log level: silent/error/warn/info/debug; aliases `none`=`silent`, `warning`=`warn` |
| `-s` | `--systemd` | `LINKSTAY_SYSTEMD` | Optional bool; bare flag = true; accepts true/false/1/0/yes/no/on/off |
| `-v` | `--version` | — | Show version |
| `-h` | `--help` | — | Show help |

Config precedence: defaults → environment variables → CLI arguments. `config_resolve()` is the single orchestration path.

## Key conventions

- `LINKSTAY_TARGET` and `--target` accept only IPv4/IPv6 literals. DNS names are intentionally rejected.
- `--systemd` / `-s` uses an optional boolean argument. A bare flag enables integration; explicit disable uses `--systemd=0`, `--systemd=false`, `-s0`, or `-sfalse`.
- `poweroff=false` (the default) is a terminal simulation path: once the threshold is reached and the simulated shutdown path completes, the process exits. Set `poweroff=true` to perform a real `systemctl poweroff`.
- Log timestamps are derived from systemd integration state, not a separate config knob: timestamps are disabled when systemd logging is enabled and enabled otherwise.
- The codebase prefers stack buffers and caller-owned error buffers over heap allocation. Many public functions follow `(..., char *restrict error_msg, size_t error_size)` and return `bool`/enum status.
- Headers form a layered hierarchy rooted at `include/common.h` (no LinkStay dependencies) and live in `include/`, while implementations live in `src/`. Each module owns a focused header; put shared enums, constants, structs, and public declarations in the header of the module that owns them, and keep monitor/runtime orchestration helpers `static` inside `monitor.c` unless another translation unit genuinely needs them.
- The monitor logic is timer/state-machine driven. `monitor.c` tracks ping reply deadlines, next-ping scheduling, and watchdog notifications as explicit timer structs instead of sleeping loops or threads. Timers use a single absolute-deadline word (UINT64_MAX = disarmed) and three inline helpers (`timer_clear`/`timer_arm_after`/`timer_step`); avoid reintroducing per-action wrapper functions.
- systemd integration lives behind `systemd_notifier_t`. There is no separate runtime-services vtable: the monitor calls `systemd_notifier_*` directly and the notifier no-ops when not enabled. If another integration is needed, add a sibling notifier module and a small dispatcher in `monitor.c` — do not resurrect a generic vtable for a single backend.
