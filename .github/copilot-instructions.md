# LinkStay Copilot Instructions

## Build, test, and lint commands

- Build the binary with `make`. The output is `bin/LinkStay`.
- Build a stripped release binary with `make release`.
- Run linters with `make lint` (`cppcheck` + `clang-tidy`). At the current baseline this command completes but reports existing findings, so do not assume the repository is lint-clean before your changes.
- The test suite (`test.sh`) is a local-only file not tracked in the repository. If available locally, run it with `./test.sh` (basic), `sudo ./test.sh --gray` (process-level), `sudo ./test.sh --gray-systemd` (systemd-level), or `sudo ./test.sh --all`.

## High-level architecture

- `src/main.c` is a thin entry point: `config_resolve()` parses defaults, environment, and CLI; `linkstay_ctx_init()` wires runtime state; `linkstay_reactor_run()` owns the monitor lifecycle.
- `src/config.c` is a single configuration module: defaults, environment parsing, getopt-based CLI parsing, `--help` / `--version`, validation, and config debug rendering live together.
- `src/monitor.c` is the core of the program. It implements the reactor loop, metrics, signal handling, ping scheduling, reply deadlines, shutdown countdowns, and systemd watchdog scheduling in one place. The loop uses `poll()` plus `signalfd`, not threaded workers.
- `src/icmp.c` owns raw-socket ICMP send/receive logic, packet construction, reply matching, and optional BPF socket filters for IPv4/IPv6 traffic.
- `src/shutdown.c` is the shutdown backend layer. It invokes `systemctl poweroff` for `true-off`, preserves dry-run and log-only behavior, and uses `posix_spawn()` plus startup observation instead of shelling out.
- `src/systemd.c` implements `sd_notify`-style integration directly over the notify socket. `systemd/LinkStay.service` expects this with `Type=notify`, `NotifyAccess=main`, and `WatchdogSec=30`; the shipped sample unit defaults to `LINKSTAY_MODE=log-only` so monitoring remains persistent until operators explicitly switch to `true-off`.
- `linkstay_ctx_t` in `src/linkstay.h` is the shared runtime object: config, resolved destination address, logger, metrics, ICMP state, and runtime services live there.

## CLI options and environment variables

All short options are lowercase and mnemonic:

| Short | Long | Env var | Description |
|-------|------|---------|-------------|
| `-t` | `--target` | `LINKSTAY_TARGET` | IP literal target (no DNS) |
| `-i` | `--interval` | `LINKSTAY_INTERVAL` | Ping interval (seconds) |
| `-n` | `--threshold` / `--fail-threshold` | `LINKSTAY_THRESHOLD` | Consecutive failure threshold; env alias `LINKSTAY_FAIL_THRESHOLD` is also accepted |
| `-w` | `--timeout` | `LINKSTAY_TIMEOUT` | Ping timeout (milliseconds) |
| `-m` | `--mode` | `LINKSTAY_MODE` | Shutdown mode: dry-run/true-off/log-only |
| `-d` | `--delay` | `LINKSTAY_DELAY` | Shutdown countdown (minutes) |
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
- `src/linkstay.h` is the central contract header. Keep shared enums, constants, structs, and true public declarations there, but keep monitor/runtime orchestration helpers inside `monitor.c` unless another translation unit genuinely needs them.
- The monitor logic is timer/state-machine driven. `monitor.c` tracks ping reply deadlines, shutdown countdowns, next-ping scheduling, and watchdog notifications as explicit timer structs instead of sleeping loops or threads.
- systemd support is intentionally abstracted through `runtime_services_t`. New runtime integrations should fit that abstraction instead of adding ad hoc branches throughout the monitor loop.
- The runtime_services function pointers use type-safe `void *` wrapper functions (not casts) to avoid UB per C23 §6.5.2.2. Follow this pattern for new backends.
- Tests are shell-driven (`test.sh`, local only) and include gray-box harnesses that compile temporary C files which `#include` `src/monitor.c` or `src/shutdown.c` directly to exercise internal/static behavior.
- The test suite enforces non-obvious product behavior from the README and service file, such as CLI/env precedence, timestamp behavior, service timeout settings, and shutdown failure semantics. When changing those areas, update both the implementation and the test expectations together.
