# linkstay — Engineering Contract

This file is the binding engineering contract for AI assistants and humans
editing this repository. It answers three questions: **what must not change**
(invariants and contracts), **where code belongs** (module boundaries), and
**how a change is proven done** (verification commands).

User-facing behavior is documented in `README.md`. Structure and rules live
here. Keep `README.md` about what the service does, and this file about how the
code must be built and changed.

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
make test            # unit suite + shell suite; shell baseline: 37 pass, 1 skip
make lint            # cppcheck + clang-tidy: zero findings
make shellcheck      # ShellCheck tests/*.sh: zero findings
make sanitize-test   # ASan+UBSan unit + shell suites
make check           # runs test + lint + shellcheck + sanitize-test
```

`tests/unit.c` currently runs 138 assertions over `base`/`addr`/`log`/`loop`/
`stats` and the environment-only `opts` resolver. Live ICMP tests are skipped
when the binary lacks `CAP_NET_RAW`.

Run them inside a user + network namespace with the recipe in `README.md`
("测试"); the full suite then reports `Pass: 61   Fail: 0   Skip: 0`
(without live tests: `Pass: 37   Fail: 0   Skip: 1`). The shell suite requires
`python3` for its NOTIFY_SOCKET receiver. If you changed anything on the probe
or event-loop path, run the live suite and say so explicitly. Never report a
skipped test as a pass. CI runs `make check` on every push and pull request
(`.github/workflows/ci.yml`).

## 3. Hard invariants

### 3.1 Runtime shape

- **systemd-only.** `linkstay` is a service, not a CLI. `main` has no
  arguments; there is no option parser, `--help`, `--version`, daemonize, or
  foreground/systemd switch. Configuration comes only from `LINKSTAY_*`
  environment variables supplied by the unit and `/etc/linkstay.conf`.
- **NOTIFY_SOCKET is required.** `ls_notify_init()` fails when
  `NOTIFY_SOCKET` is absent or unusable, and `ls_app_init()` treats that as
  fatal. A broken notify channel must never silently run a service that systemd
  cannot supervise.
- **journald owns timestamps.** Log lines are `[LEVEL] message`; there is no
  timestamp formatting or timestamp configuration.
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
  (`_POSIX_C_SOURCE=200809L`, `_DEFAULT_SOURCE`) are defined by the Makefile;
  do not add header fallbacks for them.
- **Release defaults.** LTO is always enabled; `-march=native` stays opt-in
  (`NATIVE=1`) so default binaries remain deployable on other machines of the
  same architecture.
- **C23 is enforced, not assumed.** The Makefile compiles with
  `-std=c23 -pedantic-errors`, and `base.h` has a `__STDC_VERSION__` guard that
  fails the build under an older standard. Vendor extensions are limited to an
  explicit allow-list: `[[gnu::...]]` attributes and the GCC/Clang
  `__builtin_expect` used by `LS_LIKELY`/`LS_UNLIKELY`. The build targets
  GCC/Clang; adding another compiler extension is a design change.
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
| `static const` data | `UPPER_SNAKE`, no prefix | `ENV_SPECS`, `SHUTDOWN_ARGV`, `WATCHED_SIGNALS` |
| Environment variable | `LINKSTAY_` + upper snake | `LINKSTAY_POWEROFF` |

The `ls_` prefix must mean "reachable from another translation unit". A helper
that is `static` in a `.c` file never gets the prefix; a `static inline` helper
published in a header does. Enum constants (`LS_LOG_DEBUG`,
`LS_ICMP_RECV_MATCHED`) follow the macro convention even when the enum type is
private.

### 3.3 Error handling

- Fallible public functions return `bool` and write to a caller-owned
  `ls_err_t` via `ls_err_set()`. Never invent `(char *buf, size_t len)` error
  out-parameters, and never return `-errno` as the only signal.
- Results with more than two meaningful states use a small enum, not a bool:
  `ls_icmp_recv_status_t`, `ls_icmp_send_status_t`. Do not overload `errno`.
- Messages are complete sentences without a trailing period, name the offending
  source and value, and state the accepted form, e.g.
  `Invalid value for LINKSTAY_TIMEOUT: 1m (use a positive integer suffixed 's'
  or 'ms')`.
- `ls_err_t` carries the *reason*; the caller supplies the action. A module
  message must not open with "Failed to ..." when its caller already prefixes
  the operation. Syscall failures pass `strerror(errno)` through verbatim, so
  `app_fail()` and `main()` read as "<action>: <reason>".
- Public entry points validate caller-owned arguments they can meaningfully
  reject (`ls_app_init`, `ls_loop_add_fd`, `ls_opts_resolve`, ...). Pure value
  helpers (`ls_stats_*`, `ls_format_*`, `ls_timer_*`) and the app-owned
  `ls_notify_*` channel assume valid pointers. Private `static` helpers do not
  re-check pointers.
- Modules never write to stdio. `log.c` owns log output (`fprintf` to stderr);
  everything else reports through an `ls_log_t` or an `ls_err_t`.
- `restrict` appears only on public declarations and the definitions that
  implement them. Private helpers never use it.

### 3.4 Time

- Every internal duration is milliseconds in a `uint64_t` (`interval_ms`,
  `timeout_ms`, timer deadlines). Nanoseconds are confined to the OS boundary:
  `ls_now_ns` for latency and the `nanosleep` conversion.
- Use the unit constants in `base.h` (`LS_NS_PER_MS`, `LS_NS_PER_SEC`,
  `LS_US_PER_MS`, `LS_MS_PER_SEC`). No ad-hoc `* 1000`, no raw nanosecond
  literals.
- Derived durations use the saturating helpers `ls_add_sat` / `ls_mul_sat`.
- The only duration parser is `apply_duration()` reached through `ENV_SPECS`:
  both `LINKSTAY_INTERVAL` and `LINKSTAY_TIMEOUT` require a lowercase `s` or
  `ms` suffix, and the result is always stored in milliseconds. Bare numbers
  and a leading `+` are rejected. Only one cross-field rule exists:
  `timeout_ms < interval_ms`.
- Monotonic time (`CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_COARSE`) is used for all
  logic. Wall-clock time is not read by the program.
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
  directly after the include guard.
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
| `src/log.h` `.c` | `ls_log_t`, level filtering, the project's only stderr writer | any domain concept (probes, addresses, config) |
| `src/addr.h` `.c` | IPv4/IPv6 literal classification and conversion, the "DNS disabled" rule | sockets, configuration, timers |
| `src/opts.h` `.c` | `ls_opts_t`, the `ENV_SPECS` table, defaults/env resolution, cross-field validation | raw sockets, timers, the event loop, ICMP details |
| `src/loop.h` `.c` | `poll(2)` loop, fixed fd slots, absolute-deadline timer slots, signalfd channel | probes, ICMP, addresses, configuration |
| `src/icmp.h` `.c` | raw socket, best-effort BPF filter, echo construction, reply matching, send/receive status | address syntax, timers, configuration, log policy |
| `src/notify.h` `.c` | the required `sd_notify` channel and protocol (READY/STATUS/WATCHDOG/STOPPING), NOTIFY_SOCKET/WATCHDOG_USEC/WATCHDOG_PID | when a message is appropriate (that is app policy) |
| `src/stats.h` `.c` | cumulative probe counters, success rate, latency aggregates, uptime | probes, sockets, timers, configuration |
| `src/app.h` `.c` | probe lifecycle, threshold action, startup/shutdown banners, message policy | nothing above it; it is the top of the stack |
| `src/main.c` | entry point: resolve the environment, run the app, report a fatal startup error, map the result to an exit code | `loop`, `icmp`, `notify`, and every app-internal type; it uses only `opts`/`app` entry points plus `log` |

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

### 5.1 Environment configuration

`opts.c`'s `ENV_SPECS` table is the single source of truth for the supported
variables. Adding a setting means: one `ENV_SPECS` row, one `apply_*` adapter,
one `README.md` table row, one `/etc/linkstay.conf` example line, and tests.
There is no CLI and no precedence layer: defaults, then present environment
variables, then validation.

- Variables: `LINKSTAY_TARGET`, `LINKSTAY_INTERVAL`, `LINKSTAY_THRESHOLD`,
  `LINKSTAY_TIMEOUT`, `LINKSTAY_POWEROFF`, `LINKSTAY_LOG_LEVEL`.
- Boolean values are `true` and `false` only, compared case-insensitively.
  Numeric and shell-style aliases (`1`, `0`, `yes`, `no`, `on`, `off`) are
  rejected on purpose: one spelling per state.
- Exit codes: `0` for SIGTERM/SIGINT shutdown and the completed dry-run
  threshold path; `1` for every fatal error (environment/validation error,
  missing notify channel, socket setup failure, event-loop failure, fatal
  send/receive error).
- `NOTIFY_SOCKET` is required; there is no standalone fallback. Timestamps are
  always omitted because journald adds them.

### 5.2 Probe accounting and the threshold action

- One probe is outstanding at a time; the armed reply timer is the single
  source of truth for that state, so the ping timer never overlaps a live
  probe.
- A **failure** is counted for a reply timeout *and* for routing/link-level
  send failures (`ENETUNREACH`, `EHOSTUNREACH`, `ENETDOWN`, `EHOSTDOWN`,
  `ENETRESET`, `ENOBUFS`, `EAGAIN`). Those paths must keep counting toward
  `LINKSTAY_THRESHOLD`; do not turn them back into fatal exits. Socket-misuse
  errors (`EPERM`, `EBADF`, `EINVAL`, short sends, ...) stay fatal.
- A success resets the consecutive-failure counter and updates statistics in
  `stats.c`.
- On reaching the threshold: `LINKSTAY_POWEROFF=false` logs the dry-run and
  exits `0`; `LINKSTAY_POWEROFF=true` runs
  `/usr/bin/systemctl --no-block poweroff` through `posix_spawn` (stdio to
  `/dev/null`, explicit clean environment), observes the child for at most 1 s,
  and on failure keeps monitoring with the failure count preserved.
  `poweroff=true` requires `/usr/bin/systemctl` and `/run/systemd/system`,
  validated in `opts.c`.
- Statistics (SIGUSR1 and shutdown) report uptime, totals, success rate, and
  min/avg/max latency from integral microsecond accumulators in `ls_stats_t`,
  formatted with `ls_format_uptime()` / `ls_format_latency()`. The accumulators
  must keep `min <= avg <= max`; the "no sample" sentinel is `UINT64_MAX`, so
  the max accumulator has to clear it explicitly on the first success (the live
  suite asserts the ordering, and the unit suite covers the saturating counters
  and aggregates directly).

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
- `ls_notify_init()` returns `bool`: failure means `NOTIFY_SOCKET` is missing,
  malformed, or cannot be connected. The app treats it as fatal; do not
  reintroduce an "inactive but continue" fallback.
- `ls_notify_t.opened` is the ownership flag that makes
  `ls_notify_destroy()` safe on a zero-initialized struct. Do not close a
  descriptor based only on `sockfd >= 0` (zero-initialized `sockfd` is `0`).
- Notifier messages: `READY=1` at startup, `STATUS=` updates (deduplicated
  within 2 s and capped at `LS_NOTIFY_STATUS_SIZE`), `WATCHDOG=1` at
  `WATCHDOG_USEC / 2000` ms (half the deadline), `STOPPING=1` on every exit
  path. The watchdog stays disabled when `WATCHDOG_PID` does not match
  `getpid()` or when `WATCHDOG_USEC` is missing or malformed (a leading `-`
  must not wrap around into a huge deadline).
- The unit's sandboxing is part of the product: raw ICMP needs `CAP_NET_RAW`;
  `AF_NETLINK` is intentionally excluded. Do not weaken these settings without
  an explicit request.
- `make install` installs the binary, unit, and `/etc/linkstay.conf`;
  `make uninstall` preserves the configuration file. Keep the unit's
  `@BINDIR@`/`@CONFIG_PATH@` placeholders substituted by the Makefile.

## 6. Change protocol

**Ask the user first** before changing: environment variable names, defaults,
exit codes, shutdown/poweroff semantics, the systemd unit's security posture,
install paths, or anything else `README.md` documents. These are user-visible
contracts, and the unit's `LINKSTAY_POWEROFF=true` default is a deliberate
service behavior.

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
tests/run_tests.sh          shell-suite entry point and global notify receiver
tests/lib.sh                assertions, process/NOTIFY helpers
tests/config.sh             environment/validation/systemd-only cases
tests/live.sh               live ICMP, signals, threshold cases
tests/notify.sh             NOTIFY_SOCKET protocol cases (sourced by live.sh)
systemd/linkstay.service    unit template (@BINDIR@/@CONFIG_PATH@)
systemd/linkstay.conf       /etc/linkstay.conf example
.github/workflows/ci.yml    CI: make check on push/pull_request
```

`bin/` and `bin-sanitize/` are build outputs and are gitignored; `make clean`
removes both. `.vscode/` is local editor state and is gitignored on purpose —
do not commit IDE settings.
