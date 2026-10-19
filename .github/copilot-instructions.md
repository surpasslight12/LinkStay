# linkstay — Engineering Contract

This file is the binding engineering contract for AI assistants and humans
editing this repository. It answers three questions: **what must not change**
(invariants and contracts), **where code belongs** (module boundaries), and
**how a change is proven done** (verification commands).

User-facing behavior is documented in `README.md`. Structure and rules live
here. Do not duplicate one into the other: keep `README.md` about what the
program does, and this file about how the code must be built and changed.

## 1. Authority

- An explicit user request wins over this file. If it conflicts with a rule
  below, follow the user and state in your answer which rule was bypassed.
- Otherwise this file wins over conventions you infer from the code. If the
  code contradicts a rule, treat the code as a bug and say so.
- Do not create additional AI-tool files (`AGENTS.md`, `CLAUDE.md`,
  `.cursor/rules`, `.github/instructions/*`, ...). This file is the single
  contract. Per-machine overrides belong in
  `.github/copilot-instructions.local.md`, which is gitignored.
- Do not add a `LICENSE` file or license badge; their absence is deliberate.
- Do not commit formatting or analyzer configuration (`.clang-format`,
  `.clang-tidy`, `.editorconfig` — all gitignored). The style contract in
  section 3.5 is the source of truth; `make lint` supplies its checks on the
  command line.

## 2. Definition of done

A change is done only when every command below passes and you have reported the
result. "It compiles" is not done.

```bash
make                 # must produce zero compiler warnings
make test            # unit suite + shell suite; shell baseline: 180 pass, 1 skip
make lint            # cppcheck + clang-tidy: zero findings
make shellcheck      # ShellCheck tests/*.sh: zero findings
make sanitize-test   # ASan+UBSan unit + shell suites; required for loop/icmp/app changes
make check           # runs test + lint + shellcheck + sanitize-test
```

`tests/unit.c` currently runs 170 assertions over `base`/`addr`/`log`/`loop`/
`stats` and the `opts` resolver. Live ICMP tests are skipped when the binary
lacks `CAP_NET_RAW`.
Run them inside a user + network namespace with the recipe in `README.md`
("测试说明"); the full suite then reports `Pass: 208   Fail: 0   Skip: 0`
(without live tests: `Pass: 180   Fail: 0   Skip: 1`). If you changed anything
on the probe or event-loop path, run the live suite and say so explicitly.
Never report a skipped test as a pass. CI runs `make check` on every push and
pull request (`.github/workflows/ci.yml`).

## 3. Hard invariants

### 3.1 Runtime shape

- **Zero dynamic allocation.** No `malloc`/`calloc`/`realloc`/`free`, no
  `strdup`/`asprintf`/`open_memstream`. State lives in fixed-size structs
  (`ls_app_t`, `ls_icmp_t`) or stack buffers. Adding a heap allocation is a
  design change, not a refactor.
- **Single thread, single process.** No `pthread_*`, no `fork`/`vfork`, no
  subprocess other than `posix_spawn` of `/usr/bin/systemctl --no-block
  poweroff`. The only `nanosleep` is the bounded (1 s) shutdown-command
  observation window in `app.c`; nothing else may block the loop.
- **Event-loop driven.** All periodic work is a timer slot in `loop.c` armed by
  `app.c`. Never add a sleep-loop or a signal handler that does work.
- **Linux, libc, C23 only.** No third-party libraries, no vendored code, no
  code generation, no build system beyond the `Makefile`. Feature-test macros
  (`_POSIX_C_SOURCE=200809L`, `_DEFAULT_SOURCE`) are defined in the Makefile and
  fall back to the same values in `base.h`; keep both.
- **Portable release defaults.** LTO is always enabled; `-march=native` stays
  opt-in (`NATIVE=1`) so default
  binaries remain deployable on other machines of the same architecture.
- **C23 is enforced, not assumed.** The Makefile compiles with
  `-std=c23 -pedantic-errors`, and `base.h` has a `__STDC_VERSION__` guard that
  fails the build under an older standard. Vendor extensions are limited to an
  explicit allow-list: `[[gnu::...]]` attributes (valid C23 attribute syntax
  with an implementation-defined namespace) and the GCC/Clang
  `__builtin_expect` used by `LS_LIKELY`/`LS_UNLIKELY`. The build targets
  GCC/Clang; adding another compiler extension, or a C11/C17 compatibility
  shim, is a design change.
- **Prefer the standard facility.** `<stdckdint.h>` (`ckd_mul`) is used for
  overflow-checked arithmetic, `nullptr`/`bool`/`static_assert`/`alignas` are
  keywords, and unused parameters use `[[maybe_unused]]` instead of `(void)`
  casts. `(void)expr` remains correct for deliberately discarding a return
  value.
- **Warnings are errors in spirit.** The build must stay warning-free under the
  Makefile's `WARN_CFLAGS` (`-Wconversion -Wsign-conversion -Wshadow
  -Wpedantic ...`), and `make lint` must stay at zero findings.

### 3.2 Naming and placement

| Kind | Convention | Examples |
|------|-----------|----------|
| Public symbol (declared in a header) | `ls_` prefix | `ls_app_run`, `ls_icmp_send_echo` |
| Public inline helper (header-only) | `ls_` prefix, `static inline` | `ls_timer_step`, `ls_loop_now` |
| Public type | `ls_*_t` | `ls_opts_t`, `ls_stats_t`, `ls_icmp_recv_status_t` |
| `static` function in a `.c` file | plain verb-noun, no prefix | `probe_failed`, `reply_matches`, `notify_status` |
| Preprocessor macro | `LS_` prefix, also when file-private | `LS_PACKET_SIZE`, `LS_DEFAULT_TARGET` |
| `static const` data | `UPPER_SNAKE`, no prefix | `OPT_SPECS`, `SHUTDOWN_ARGV`, `WATCHED_SIGNALS` |
| Environment variable | `LINKSTAY_` + upper snake | `LINKSTAY_POWEROFF` |

The `ls_` prefix must mean "reachable from another translation unit". A helper
that is `static` in a `.c` file never gets the prefix; a `static inline` helper
published in a header does. Enum constants (`LS_PROBE_IDLE`,
`LS_ICMP_RECV_MATCHED`) follow the macro convention even when the enum type is
private.

### 3.3 Error handling

- Fallible public functions return `bool` and write to a caller-owned
  `ls_err_t` via `ls_err_set()`. Never invent `(char *buf, size_t len)` error
  out-parameters, and never return `-errno` as the only signal.
- Results with more than two meaningful states use a small enum, not a bool:
  `ls_icmp_recv_status_t`, `ls_icmp_send_status_t`, `ls_notify_state_t`. Do not
  overload `errno`.
- Messages are complete sentences without a trailing period, name the offending
  source and value, and state the accepted form, e.g.
  `Invalid value for --timeout: 1m (use a positive integer suffixed 's' or
  'ms')`.
- `ls_err_t` carries the *reason*; the caller supplies the action. A module
  message must not open with "Failed to ..." when its caller already prefixes
  the operation (that produced "Failed to send ICMP echo: Failed to send
  packet: Network is unreachable"). Syscall failures pass `strerror(errno)`
  through verbatim, so `app_fail()` and `main()` read as
  "<action>: <reason>".
- Public entry points validate caller-owned arguments they can meaningfully
  reject (`ls_app_init`, `ls_loop_add_fd`, `ls_opts_resolve`, ...). Pure value
  helpers (`ls_stats_*`, `ls_format_*`, `ls_timer_*`) and the app-owned
  `ls_notify_*` channel assume valid pointers. Private `static` helpers do not
  re-check pointers.
- Modules never write to stdio. `log.c` owns log output (`fprintf` to stderr)
  and `opts.c` owns `--help`/`--version` printing; everything else reports
  through an `ls_log_t` or an `ls_err_t`.
- `restrict` appears only on public declarations and the definitions that
  implement them. Private helpers never use it.

### 3.4 Time

- Every internal duration is milliseconds in a `uint64_t` (`interval_ms`,
  `timeout_ms`, timer deadlines). Nanoseconds are confined to the OS
  boundary: `ls_now_ns` for latency, log timestamp fractions, and the
  `nanosleep` conversion.
- Use the unit constants in `base.h` (`LS_NS_PER_MS`, `LS_NS_PER_SEC`,
  `LS_US_PER_MS`, `LS_MS_PER_SEC`). No ad-hoc `* 1000`, no raw nanosecond
  literals.
- Derived durations use the saturating helpers `ls_add_sat` / `ls_mul_sat`.
- The only user-facing duration parser is `apply_duration()` reached through
  `OPT_SPECS`: both `--interval` and `--timeout` require a lowercase `s` or `ms`
  suffix, and the result is always stored in milliseconds. Bare numbers and
  a leading `+` are rejected. Only one cross-field rule exists:
  `timeout_ms < interval_ms`.
- Monotonic time (`CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_COARSE`) for all logic;
  `CLOCK_REALTIME` only for log timestamps in `log.c`.
- Format user-visible durations with `ls_format_duration()` (whole seconds as
  `10s`, otherwise `1500ms`). Latency values use `ls_format_latency()` so
  sub-millisecond links do not collapse to `0.00ms`, and elapsed times use
  `ls_format_uptime()` (`1h2m3s`). Do not hand-roll `%.2fms` or raw second
  counts in log lines.

### 3.5 Style

- Two-space indent, roughly 80 columns, no tabs, no trailing whitespace.
- `/* ... */` comments only; `//` is not used.
- Every `.h` and `.c` file carries a file header comment: `/*` alone on its own
  line, a ` * name — one-line boundary description.` line, optional further ` *`
  detail lines, and ` */` alone. `.c` files open with it; headers place it
  directly after the include guard and feature-test macros.
- Section markers are single-line `/* ---- Name ---- */`. Other multi-line
  comments open on the `/*` line and close inline on the last text line; do not
  use `=`-rule separators or a `*/`-alone closer.
- Adjacent string literals concatenate directly. Backslash line continuation is
  allowed only inside `#define`.
- Prefer `sizeof(*ptr)` over `sizeof(struct X)` whenever a pointer to that type
  is in scope.
- C23 idioms: `nullptr`, `bool`, `static_assert`, designated initializers,
  compound-literal assignment for whole-struct (re)initialization, declarations
  at first use.
- Mark fallible/status-returning public functions `[[nodiscard]]` (the error
  reporter `ls_err_set` is the deliberate exception, since callers return it
  directly), printf-like helpers `[[gnu::format(printf, m, n)]]`, and error
  loggers `LS_COLD`.
- Branch hints (`LS_LIKELY`/`LS_UNLIKELY`) belong only on genuinely hot or cold
  paths, not as decoration.

## 4. Module map and boundaries

Dependency direction is strictly one-way; a module may include only headers
from the layers above it:

```
layer 0  base                                    (no project dependencies)
layer 1  log, addr, notify, stats                (base only)
layer 2  opts   (base, log, addr)
         loop   (base, log)
         icmp   (base)
layer 3  app                                     (all of the above)
layer 4  main                                    (opts, app, log)
```

| File | Owns | Must not know about |
|------|------|---------------------|
| `src/base.h` `.c` | program identity, exit codes, error type, monotonic clock, saturating math, duration formatting | every other module; it is stateless and has no I/O beyond the clock |
| `src/log.h` `.c` | `ls_log_t`, level filtering, timestamp formatting, the project's only stderr writer | any domain concept (probes, addresses, config) |
| `src/addr.h` `.c` | IPv4/IPv6 literal classification and conversion, the "DNS disabled" rule | sockets, configuration, timers |
| `src/opts.h` `.c` | `ls_opts_t`, the `OPT_SPECS` table, defaults/CLI/env resolution, cross-field validation | raw sockets, timers, the event loop, ICMP details |
| `src/loop.h` `.c` | `poll(2)` loop, fixed fd slots, absolute-deadline timer slots, signalfd channel | probes, ICMP, addresses, configuration |
| `src/icmp.h` `.c` | raw socket, best-effort BPF filter, echo construction, reply matching, send/receive status | address syntax, timers, configuration, log policy |
| `src/notify.h` `.c` | the `sd_notify` channel and protocol (READY/STATUS/WATCHDOG/STOPPING), NOTIFY_SOCKET/WATCHDOG_USEC/WATCHDOG_PID | when a message is appropriate (that is app policy) |
| `src/stats.h` `.c` | cumulative probe counters, success rate, latency aggregates, uptime | probes, sockets, timers, configuration |
| `src/app.h` `.c` | probe state machine, threshold action, startup/shutdown banners, message policy | nothing above it; it is the top of the stack |
| `src/main.c` | entry point: resolve options, run the app, report a fatal startup error, map the result to an exit code | `loop`, `icmp`, `notify`, and every app-internal type; it uses only `opts`/`app` entry points plus `log` |

Rules:

- A module never includes another module's `.c`, and never reaches "upward"
  (e.g. `icmp.c` must not call into `app.c`).
- Shared enums/constants/structs live in the header of the module that owns
  them. Private helper types stay in the `.c` file.
- A rule that two modules would otherwise re-implement gets one owner: address
  literals in `addr`, output in `log`, the systemd channel in `notify`, probe
  counters in `stats`. If you need the same knowledge in two places, move it
  down instead of copying it.
- Test-only or debug-only accessors are not added to headers; use the existing
  debug logging instead.
- Add a new module only for a genuinely new mechanism (a transport, an
  integration backend). Policy and orchestration belong in `app.c`. Do not
  introduce a runtime-services vtable or function-pointer indirection for a
  single implementation.

## 5. Contracts

### 5.1 CLI and environment (user-visible)

`opts.c`'s `OPT_SPECS` table is the single source of truth: it generates the
`getopt_long` tables (so the short option string can never drift from the long
options), drives the environment layer, supplies the names used in error
messages, and owns the per-option help printers used by `--help`. Adding an
option means: one `OPT_SPECS` row, one `apply_*` adapter, one `help_*` printer,
a `README.md` table row, and tests. The user-facing option table lives in
`README.md`; if the two ever disagree, `OPT_SPECS` (the code) wins and both
documents must be corrected.

- Effective precedence: CLI > environment > defaults. The CLI is applied first
  and an env var is consulted only when its option was not seen, so an invalid
  env value can never defeat a valid CLI flag.
- `--help`/`--version` are detected by a getopt pre-scan and win over invalid
  arguments and invalid environment values. Keep that property.
- `-p`/`-s` require an explicit `true`/`false` argument; bare flags are
  rejected. `-sfalse`/`--systemd=false` and the `true` equivalents are the
  documented forms. A separated value (`--systemd false`) is consumed as the
  option argument by getopt.
- Boolean values are `true` and `false` only, compared case-insensitively.
  Numeric and shell-style aliases (`1`, `0`, `yes`, `no`, `on`, `off`) are
  rejected on purpose: one spelling per state. Do not reintroduce aliases
  without an explicit user request.
- Exit codes: `0` for help/version, SIGTERM/SIGINT shutdown, and the completed
  dry-run threshold path; `1` for every fatal error (option/validation error,
  socket setup failure, event-loop failure, fatal send/receive error).
- Log timestamps are derived, not configured: disabled when systemd integration
  is enabled (journald already timestamps), enabled otherwise.

### 5.2 Probe accounting and the threshold action

- One probe is outstanding at a time (`LS_PROBE_IDLE` ↔
  `LS_PROBE_AWAIT_REPLY`); the ping timer keeps its phase but never overlaps a
  live probe.
- A **failure** is counted for a reply timeout *and* for routing/link-level
  send failures (`ENETUNREACH`, `EHOSTUNREACH`, `ENETDOWN`, `EHOSTDOWN`,
  `ENETRESET`, `ENOBUFS`, `EAGAIN`). Those paths must keep counting toward
  `--threshold`; do not turn them back into fatal exits. Socket-misuse errors
  (`EPERM`, `EBADF`, `EINVAL`, short sends, ...) stay fatal.
- A success resets the consecutive-failure counter and updates statistics in
  `stats.c`.
- On reaching the threshold: `poweroff=false` logs the dry-run and exits `0`;
  `poweroff=true` runs `/usr/bin/systemctl --no-block poweroff` through
  `posix_spawn` (stdio to `/dev/null`, explicit clean environment), observes
  the child for at most 1 s, and on failure keeps monitoring with the failure
  count preserved. `poweroff=true` requires `/usr/bin/systemctl` and
  `/run/systemd/system`, validated in `opts.c`.
- Statistics (SIGUSR1 and shutdown) report uptime, totals, success rate, and
  min/avg/max latency from integral microsecond accumulators in
  `ls_stats_t`, formatted with `ls_format_uptime()` / `ls_format_latency()`.
  The accumulators must keep `min <= avg <= max`; the "no sample" sentinel is
  `UINT64_MAX`, so the max accumulator has to clear it explicitly on the first
  success (the live suite asserts the ordering, and the unit suite covers the
  saturating counters and aggregates directly).

### 5.3 Event loop and timers

- Timer slots hold an absolute monotonic deadline; `UINT64_MAX` means disarmed.
  A fired callback **must** re-arm (`ls_timer_step`) or disarm
  (`ls_timer_disarm`) its timer, otherwise the loop spins.
- `ls_timer_step(timer, 0, now)` disarms instead of re-arming at `now`; keep
  that guard.
- Registration order is firing order. `app.c` registers watchdog heartbeat,
  then reply deadline, then ping scheduler, which guarantees a probe timeout is
  processed before the next probe is due.
- Fixed capacities (`LS_LOOP_MAX_FDS`, `LS_LOOP_MAX_TIMERS`) are deliberate: no
  dynamic registration, no removal. Raise the constant if a real need appears.

### 5.4 ICMP transport

- IPv4 raw sockets deliver the IPv4 header (skip `ip_hl * 4`, which also covers
  IPv4 options); IPv6 raw sockets deliver the ICMPv6 payload only. This
  asymmetry is standard Linux behavior — do not "fix" it.
- The BPF filter is an optimization, never a requirement: attach failure is
  recorded in `bpf_errno` and logged as a warning while the monitor continues.
- The echo sequence is never `0`, and the identifier is `pid & 0xffff` (clamped
  to `1` if zero), so an idle/cleared probe can never match a live reply.
- The IPv6 checksum is left `0` because the kernel computes it; the IPv4
  checksum is computed with the RFC 1071 helper.

### 5.5 systemd integration

- `systemd/linkstay.service` expects `Type=notify`, `NotifyAccess=main`,
  `WatchdogSec=30`. Keep them consistent with the notifier.
- `ls_notify_init()` reports one of three states: `LS_NOTIFY_ACTIVE`,
  `LS_NOTIFY_INACTIVE` (NOTIFY_SOCKET unset — systemd is not supervising), or
  `LS_NOTIFY_FAILED` (NOTIFY_SOCKET set but unusable). Only FAILED is an error,
  and even then it is non-fatal: `app.c` logs a warning and monitoring
  continues. Keep that property — a broken notify channel must never abort a
  network monitor.
- Notifier messages: `READY=1` at startup, `STATUS=` updates (deduplicated
  within 2 s and capped at `LS_NOTIFY_STATUS_SIZE`), `WATCHDOG=1` at
  `WATCHDOG_USEC / 2000` ms (half the deadline), `STOPPING=1` on every exit
  path. The watchdog stays disabled when `WATCHDOG_PID` does not match
  `getpid()` or when `WATCHDOG_USEC` is missing or malformed (a leading `-`
  must not wrap around into a huge deadline).
- Without `NOTIFY_SOCKET` (or with `--systemd=false`) every notifier send is a
  no-op; `app.c` may query `ls_notify_enabled()` for diagnostics, but need not
  branch before sending.
- The unit's sandboxing is part of the product: raw ICMP needs `CAP_NET_RAW`;
  `AF_NETLINK` is intentionally excluded. Do not weaken these settings without
  an explicit request.

## 6. Change protocol

**Ask the user first** before changing: CLI/env names, defaults, exit codes,
shutdown/poweroff semantics, files or dependencies, the systemd unit's security
posture, or anything else `README.md` documents. These are user-visible
contracts, and `--poweroff=false` as the default is a safety property.

**Free to do without asking:** internal refactors that preserve behavior, dead
code removal, comment and documentation corrections that match the code, new
tests, hot-path performance work, and consistency fixes within the rules above.

**Do not do:** add heap/threads/dependencies/a license, reformat unrelated code,
mix a behavior change with a pure refactor in one change, add AI-tool files, or
leave `make check` (or any individual target it runs) failing.

When code and documentation disagree, the code is the source of truth for
behavior and this file is the source of truth for structure — fix whichever is
wrong and say which one you changed.

## 7. Repository and generated files

```
src/                        flat C modules, one owner per responsibility (section 4)
tests/unit.c                unit tests for base/addr/log/loop/stats/opts
tests/run_tests.sh          shell-suite entry point
tests/lib.sh                assertions, process/NOTIFY helpers
tests/cli.sh                CLI/env/validation/precedence cases
tests/live.sh               live ICMP, signals, threshold cases
tests/notify.sh             NOTIFY_SOCKET protocol cases (sourced by live.sh)
.github/workflows/ci.yml    CI: make check on push/pull_request
```

`bin/` and `bin-sanitize/` are build outputs and are gitignored; `make clean`
removes both. `.vscode/` is local editor state and is gitignored on purpose —
do not commit IDE settings.
