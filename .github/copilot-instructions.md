# LinkStay Copilot Instructions

## Build and lint commands

- Build the binary with `make`. The output is `bin/LinkStay`.
- Build a stripped release binary with `make release`.
- Run linters with `make lint` (`cppcheck` + `clang-tidy`). At the current baseline this command completes but reports existing findings, so do not assume the repository is lint-clean before your changes.

## High-level architecture

- `src/main.c` is a thin entry point: `config_resolve()` parses defaults, environment, and CLI; `linkstay_ctx_init()` wires runtime state; `linkstay_reactor_run()` owns the monitor lifecycle.
- `src/config.c` is a single configuration module: defaults, environment parsing, getopt-based CLI parsing, `--help` / `--version`, validation, and config debug rendering live together.
- `src/monitor.c` is the core orchestration: the reactor loop, signal handling, ping scheduling, reply deadlines, shutdown FSM, and systemd watchdog scheduling. The loop uses `poll()` plus `signalfd`, not threaded workers. Ping statistics and the runtime-services abstraction have been factored out into `src/metrics.c` and `src/runtime.c` respectively.
- `src/metrics.c` aggregates ping statistics (counts, latency extremes, uptime) behind a small value type.
- `src/runtime.c` is the runtime-services abstraction: it adapts integration backends (currently systemd) to a type-safe vtable so the monitor loop stays backend-agnostic.
- `src/icmp.c` owns raw-socket ICMP send/receive logic, packet construction, reply matching, and optional BPF socket filters for IPv4/IPv6 traffic.
- `src/shutdown.c` is the shutdown backend layer. It invokes `systemctl poweroff` for `true-off`, preserves dry-run and log-only behavior, and uses `posix_spawn()` plus startup observation instead of shelling out.
- `src/systemd.c` implements `sd_notify`-style integration directly over the notify socket. `systemd/LinkStay.service` expects this with `Type=notify`, `NotifyAccess=main`, and `WatchdogSec=30`; the shipped sample unit defaults to `LINKSTAY_MODE=log-only` so monitoring remains persistent until operators explicitly switch to `true-off`.
- Headers are organized as a layered hierarchy: `src/common.h` is the dependency-free foundation; each module owns its own header (`logger.h`, `metrics.h`, `config.h`, `icmp.h`, `shutdown.h`, `systemd.h`, `runtime.h`); `src/monitor.h` aggregates them and defines `linkstay_ctx_t`, the shared runtime object holding config, resolved destination address, logger, metrics, ICMP state, and runtime services.

## CLI options and environment variables

All short options are lowercase and mnemonic:

| Short | Long | Env var | Description |
|-------|------|---------|-------------|
| `-t` | `--target` | `LINKSTAY_TARGET` | IP literal target (no DNS) |
| `-i` | `--interval` | `LINKSTAY_INTERVAL` | Ping interval (seconds) |
| `-n` | `--threshold` / `--fail-threshold` | `LINKSTAY_THRESHOLD` | Consecutive failure threshold; env alias `LINKSTAY_FAIL_THRESHOLD` is also accepted |
| `-w` | `--timeout` | `LINKSTAY_TIMEOUT` | Ping timeout (milliseconds) |
| `-m` | `--mode` | `LINKSTAY_MODE` | Shutdown mode: dry-run/true-off/log-only |
| `-l` | `--log-level` | `LINKSTAY_LOG_LEVEL` | Log level: silent/error/warn/info/debug; aliases `none`=`silent`, `warning`=`warn` |
| `-s` | `--systemd` | `LINKSTAY_SYSTEMD` | Optional bool; bare flag = true; accepts true/false/1/0/yes/no/on/off |
| `-v` | `--version` | — | Show version |
| `-h` | `--help` | — | Show help |

Config precedence: defaults → environment variables → CLI arguments. `config_resolve()` is the single orchestration path.

## Key conventions

- `LINKSTAY_TARGET` and `--target` accept only IPv4/IPv6 literals. DNS names are intentionally rejected.
- `--systemd` / `-s` uses an optional boolean argument. A bare flag enables integration; explicit disable uses `--systemd=0`, `--systemd=false`, `-s0`, or `-sfalse`.
- `--fail-threshold` is a clearer long alias for `--threshold`. `LINKSTAY_FAIL_THRESHOLD` is accepted as an environment alias; if it conflicts with `LINKSTAY_THRESHOLD`, config resolution fails fast.
- `dry-run` is a terminal simulation path: once the threshold is reached and the simulated shutdown path completes, the process exits. Use `log-only` for persistent safe monitoring.
- Log timestamps are derived from systemd integration state, not a separate config knob: timestamps are disabled when systemd logging is enabled and enabled otherwise.
- The codebase prefers stack buffers and caller-owned error buffers over heap allocation. Many public functions follow `(..., char *restrict error_msg, size_t error_size)` and return `bool`/enum status.
- Headers form a layered hierarchy rooted at `src/common.h` (no LinkStay dependencies). Each module owns a focused header; put shared enums, constants, structs, and public declarations in the header of the module that owns them, and keep monitor/runtime orchestration helpers `static` inside `monitor.c` unless another translation unit genuinely needs them.
- The monitor logic is timer/state-machine driven. `monitor.c` tracks ping reply deadlines, next-ping scheduling, and watchdog notifications as explicit timer structs instead of sleeping loops or threads.
- systemd support is intentionally abstracted through `runtime_services_t`. New runtime integrations should fit that abstraction instead of adding ad hoc branches throughout the monitor loop.
- The runtime_services function pointers use type-safe `void *` wrapper functions (not casts) to avoid UB per C23 §6.5.2.2. Follow this pattern for new backends.
