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

## 2. Definition of done

A change is done only when every command below passes and you have reported the
result. "It compiles" is not done.

```bash
make                 # must produce zero compiler warnings
make test            # full suite; baseline: 179 pass, 1 skip (no CAP_NET_RAW)
make lint            # cppcheck + clang-tidy: zero findings
make sanitize-test   # ASan+UBSan build, same suite; required for loop/icmp/app changes
```

Live ICMP tests are skipped when the binary lacks `CAP_NET_RAW`. Without root
(or in a container) run the whole live suite inside a user + network namespace,
where the binary owns the network namespace and can create raw sockets:

```bash
unshare -Urn --map-root-user bash -c \
  'ip link set lo up; LINKSTAY_TEST_LIVE=1 LINKSTAY_TEST_UNREACHABLE_TARGET=203.0.113.1 make test'
# baseline with live tests enabled: Pass: 199   Fail: 0   Skip: 0
```

`LINKSTAY_TEST_LIVE=1` forces the live section past the `getcap` check;
`LINKSTAY_TEST_UNREACHABLE_TARGET` points the "no reply reaches the threshold"
test at an unroutable address so it exercises the `ENETUNREACH` send path
instead of a silent timeout. If you changed anything on the probe or event-loop
path, run the live suite and say so explicitly. Never report a skipped test as
a pass.

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
- **Warnings are errors in spirit.** The build must stay warning-free under the
  Makefile's `WARN_CFLAGS` (`-Wconversion -Wsign-conversion -Wshadow
  -Wpedantic ...`), and `make lint` must stay at zero findings.

### 3.2 Naming and placement

| Kind | Convention | Examples |
|------|-----------|----------|
| Public symbol (declared in a header) | `ls_` prefix | `ls_app_run`, `ls_icmp_send_echo` |
| Public inline helper (header-only) | `ls_` prefix, `static inline` | `ls_timer_step`, `ls_loop_now` |
| Public type | `ls_*_t` | `ls_opts_t`, `ls_icmp_recv_status_t` |
| `static` function in a `.c` file | plain verb-noun, no prefix | `probe_failed`, `reply_matches`, `notify_status` |
| Preprocessor macro | `LS_` prefix, also when file-private | `LS_PACKET_SIZE`, `LS_DEFAULT_TARGET` |
| `static const` data | `UPPER_SNAKE`, no prefix | `OPT_SPECS`, `BOOL_NAMES`, `SHUTDOWN_ARGV` |
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
  `ls_icmp_recv_status_t`, `ls_icmp_send_status_t`. Do not overload `errno`.
- Messages are complete sentences without a trailing period, name the offending
  source and value, and state the accepted form, e.g.
  `Invalid value for --timeout: 1m (use a positive integer, optionally suffixed
  's' or 'ms')`.
- Public entry points validate their arguments (`ls_app_init`, `ls_loop_add_fd`,
  `ls_opts_resolve`, ...). Private `static` helpers assume their arguments are
  valid and are not required to re-check pointers for null.
- Modules never write to stdio. `base.c` owns logging (`fprintf` to stderr) and
  `opts.c` owns `--help`/`--version` printing; everything else reports through
  an `ls_log_t` or an `ls_err_t`.
- `restrict` appears only on public declarations and the definitions that
  implement them. Private helpers never use it.

### 3.4 Time

- Every internal duration is milliseconds in a `uint64_t` (`interval_ms`,
  `timeout_ms`, timer deadlines). Nanoseconds exist only for latency
  measurement (`ls_now_ns`).
- Use the unit constants in `base.h` (`LS_NS_PER_MS`, `LS_US_PER_MS`,
  `LS_MS_PER_SEC`). No ad-hoc `* 1000`, no raw nanosecond literals.
- Derived durations use the saturating helpers `ls_add_sat` / `ls_mul_sat`.
- The only user-facing duration parser is `apply_duration()` reached through
  `OPT_SPECS`: a plain number keeps its historical unit (`--interval` = seconds,
  `--timeout` = milliseconds), a lowercase `s`/`ms` suffix overrides it, and the
  result is always stored in milliseconds. Only one cross-field rule exists:
  `timeout_ms < interval_ms`.
- Monotonic time (`CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_COARSE`) for all logic;
  `CLOCK_REALTIME` only for log timestamps in `base.c`.
- Format user-visible durations with `ls_format_duration()` (whole seconds as
  `10s`, otherwise `1500ms`).

### 3.5 Style

- Two-space indent, roughly 80 columns, no tabs, no trailing whitespace.
- `/* ... */` comments only; `//` is not used.
- Every file carries a file header comment: `/*` alone on its own line, a
  ` * name — one-line boundary description.` line, optional further ` *` detail
  lines, and ` */` alone. `.c` files open with it; headers place it directly
  after the include guard and feature-test macros.
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

Dependency direction is strictly one-way:

```
base.h/.c  ←  { loop, icmp, opts }  ←  app.h/.c  ←  main.c
```

| File | Owns | Must not know about |
|------|------|---------------------|
| `src/base.h` `.c` | program identity, exit codes, error type, monotonic clock, saturating math, duration formatting, leveled logging | any other linkstay module |
| `src/opts.h` `.c` | `ls_opts_t`, the `OPT_SPECS` table, defaults/CLI/env resolution, cross-field validation | sockets, ICMP, timers, the event loop |
| `src/loop.h` `.c` | `poll(2)` loop, fixed fd slots, absolute-deadline timer slots, signalfd channel | probes, ICMP, configuration, statistics |
| `src/icmp.h` `.c` | target literal parsing, raw socket, best-effort BPF filter, echo construction, reply matching | timers, configuration, log policy |
| `src/app.h` `.c` | probe state machine, statistics, threshold action, systemd notifier, startup/shutdown banners | nothing above it; it is the top of the stack |
| `src/main.c` | entry point: resolve options, run the app, report a fatal startup error, map result to exit code | `loop`, `icmp`, and every app-internal type; it uses only the `opts`/`app` entry points plus base logging |

Rules:

- A module never includes another module's `.c`, and never reaches "upward"
  (e.g. `icmp.c` must not call into `app.c`).
- Shared enums/constants/structs live in the header of the module that owns
  them. Private helper types stay in the `.c` file.
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
options), drives the environment layer, and supplies the names used in error
messages. Adding an option means: one `OPT_SPECS` row, one `apply_*` adapter,
help text, a `README.md` table row, and tests. `README.md` repeats the table
below for users; if the two ever disagree, `OPT_SPECS` (the code) wins and both
documents must be corrected.

| Short | Long | Env var | Default | Meaning |
|-------|------|---------|---------|---------|
| `-t` | `--target` | `LINKSTAY_TARGET` | `1.1.1.1` | IPv4/IPv6 literal only; DNS is rejected |
| `-i` | `--interval` | `LINKSTAY_INTERVAL` | `10s` | Probe interval; plain number = seconds |
| `-n` | `--threshold` | `LINKSTAY_THRESHOLD` | `5` | Consecutive failures before the action |
| `-w` | `--timeout` | `LINKSTAY_TIMEOUT` | `3000ms` | Reply timeout; plain number = ms; must be `< interval` |
| `-p` | `--poweroff` | `LINKSTAY_POWEROFF` | `false` | Optional bool (`true`/`false`); bare flag = true; false = dry-run |
| `-l` | `--log-level` | `LINKSTAY_LOG_LEVEL` | `info` | `silent\|error\|warn\|info\|debug` |
| `-s` | `--systemd` | `LINKSTAY_SYSTEMD` | `true` | Optional bool (`true`/`false`); bare flag = true |
| `-v` | `--version` | — | — | Print version, exit 0 |
| `-h` | `--help` | — | — | Print help, exit 0 |

- Effective precedence: CLI > environment > defaults. The CLI is applied first
  and an env var is consulted only when its option was not seen, so an invalid
  env value can never defeat a valid CLI flag.
- `--help`/`--version` are detected by a getopt pre-scan and win over invalid
  arguments and invalid environment values. Keep that property.
- `-p`/`-s` take an *optional* argument: `--systemd false` is a positional
  argument error, not a bool. The documented disable forms are `-sfalse` and
  `--systemd=false` (or `-pfalse` / `--poweroff=false`).
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
- A success resets the consecutive-failure counter and updates statistics.
- On reaching the threshold: `poweroff=false` logs the dry-run and exits `0`;
  `poweroff=true` runs `/usr/bin/systemctl --no-block poweroff` through
  `posix_spawn` (stdio to `/dev/null`, explicit clean environment), observes
  the child for at most 1 s, and on failure keeps monitoring with the failure
  count preserved. `poweroff=true` requires `/usr/bin/systemctl` and
  `/run/systemd/system`, validated in `opts.c`.
- Statistics (SIGUSR1 and shutdown) report uptime, totals, success rate, and
  min/avg/max latency from integral microsecond accumulators.

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
- Notifier messages: `READY=1` at startup, `STATUS=` updates (deduplicated
  within 2 s and capped at `LS_NOTIFY_STATUS_SIZE`), `WATCHDOG=1` at
  `WATCHDOG_USEC / 2000` ms (half the deadline), `STOPPING=1` on every exit
  path. `WATCHDOG_PID` must match `getpid()` or the watchdog stays disabled.
- Without `NOTIFY_SOCKET` (or with `--systemd=false`) every notifier call is a
  no-op; callers never branch on availability.
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
leave `make lint` / `make test` failing.

When code and documentation disagree, the code is the source of truth for
behavior and this file is the source of truth for structure — fix whichever is
wrong and say which one you changed.

## 7. Repository map

```
Makefile                        build, release, lint, test, sanitize, install
README.md                       user-facing behavior (CLI, shutdown, systemd)
src/base.h/.c                   foundation: errors, clock, logging, durations
src/opts.h/.c                   OPT_SPECS table, option resolution, validation
src/loop.h/.c                   poll(2) loop, fd slots, timers, signalfd
src/icmp.h/.c                   raw socket, BPF, echo build/match
src/app.h/.c                    probe FSM, stats, action, notifier
src/main.c                      entry point
systemd/linkstay.service        deployable unit example (sandboxed)
tests/run_tests.sh              black-box CLI/env/live/systemd-notify suite
.github/copilot-instructions.md this contract
```
