# linkstay — 高性能网络监控工具

[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**linkstay** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket + BPF 内核过滤，无需依赖系统 `ping` 命令
- **可选的真实关机**：通过布尔开关 `--poweroff` 决定是否正式关机；`false` 仅模拟，`true` 达到阈值后立即执行关机
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知；watchdog 随 systemd 自动启用
- **高性能**：默认 LTO 构建，单一二进制文件 ≈ 42 KiB（strip 后），内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：编译期 Full RELRO、PIE、Stack Canary、NX、FORTIFY_SOURCE；运行期 systemd 沙箱

## 快速开始

### 1. 构建

```bash
make               # 构建 bin/linkstay（默认启用 LTO）
make release       # 构建后 strip
make test          # 单元测试 + 功能测试
make check         # test + lint + shellcheck + sanitize-test
make sanitize-test # ASan/UBSan 构建并运行两套测试
```

### 2. 运行

```bash
# 查看帮助
./bin/linkstay --help

# 前台调试运行（不实际关机，仅模拟）
sudo ./bin/linkstay --target 1.1.1.1 --interval 10 --timeout 3000 \
     --threshold 3 --log-level debug

# 短选项示例：必选参数既可写成 “-i 5”，也可写成 “-i5”
# 带可选参数的 -p / -s 建议写成 -pfalse/-sfalse，或使用 --poweroff=false/--systemd=false
sudo ./bin/linkstay -t 1.1.1.1 -i 5 -w 1000 -n 3 -l debug -sfalse
```

### 3. 可选：注册 systemd 服务

```bash
make
sudo make install-systemd
sudo systemctl daemon-reload
sudo systemctl enable --now linkstay
```

仓库内附带的示例 unit 默认使用 `LINKSTAY_POWEROFF=false`，这样只会模拟关机；确认行为后再显式切换到 `LINKSTAY_POWEROFF=true`。

服务启动后可通过 drop-in 覆盖 `Environment=` 行来修改配置，无需改动原始 unit 文件：

```bash
sudo systemctl edit linkstay
# 在弹出的编辑器里写入：
# [Service]
# Environment="LINKSTAY_TARGET=192.168.1.1"
# Environment="LINKSTAY_POWEROFF=true"

sudo systemctl daemon-reload
sudo systemctl restart linkstay
```

查看实时日志：

```bash
journalctl -fu linkstay
```

### 4. Make 目标

| 目标 | 说明 |
|------|------|
| `make` | 构建 `bin/linkstay`（默认启用 LTO） |
| `make release` | 构建后 strip |
| `make help` | 列出全部目标和可覆盖变量 |
| `make unit-test` | 构建并运行 `tests/unit.c` 单元测试 |
| `make test` | 单元测试 + `tests/run_tests.sh` 功能测试 |
| `make lint` | cppcheck + clang-tidy，基线为零告警 |
| `make shellcheck` | 对 `tests/*.sh` 运行 ShellCheck，基线为零告警 |
| `make check` | 依次执行 `test` + `lint` + `shellcheck` + `sanitize-test`（提交前的最小验证集） |
| `make sanitize` | 以 ASan/UBSan 构建到 `bin-sanitize/`（可用 `SAN=` 覆盖） |
| `make sanitize-test` | sanitizer 构建并运行单元测试与功能测试 |
| `make install` | 安装二进制到 `$(PREFIX)/bin/linkstay`，默认 `PREFIX=/usr/local` |
| `make install-systemd` | 安装二进制和 `systemd/linkstay.service` |
| `make uninstall` | 删除已安装的二进制和 systemd unit |
| `make clean` | 清理 `bin/` 与 `bin-sanitize/` |

安装目标支持 staging，例如：

```bash
make install-systemd DESTDIR=/tmp/linkstay-stage PREFIX=/usr/local
```

默认构建不启用 `-march=native`，产物可在同架构其他机器上运行；如需为本机 CPU 调优，可使用 `make NATIVE=1`。默认启用 `-flto`，工具链不支持 LTO 时用 `make LTO=0`；切换 `NATIVE`/`LTO`/警告选项会自动触发重建（Makefile 用配置戳记）。`make check` 需要 `cppcheck`、`clang-tidy` 与 `shellcheck`。

### 5. 测试说明

`make test` 先运行 `tests/unit.c`（166 项单元断言，覆盖 base/addr/log/loop/stats/opts），再由 `tests/run_tests.sh` 驱动 shell 套件，覆盖参数/环境变量解析、优先级、错误信息、NOTIFY_SOCKET 协议与实网 ICMP（loopback、信号、阈值模拟关机）。shell 套件拆分为 `tests/lib.sh`（断言与进程/通知辅助）、`tests/cli.sh`（静态用例）、`tests/live.sh`（实网用例）和 `tests/notify.sh`（sd_notify 协议用例）。实网用例需要 `bin/linkstay` 具备 `cap_net_raw`，缺失时该组用例自动跳过并计入 `Skip`。

容器或普通用户无法用 `setcap` 时，可在用户+网络命名空间内运行完整实网用例（无需 root）：

```bash
unshare -Urn --map-root-user bash -c \
  'ip link set lo up; LINKSTAY_TEST_LIVE=1 LINKSTAY_TEST_UNREACHABLE_TARGET=203.0.113.1 make test'
```

`LINKSTAY_TEST_LIVE=1` 强制启用实网用例；`LINKSTAY_TEST_UNREACHABLE_TARGET` 指定“不可达目标”用例的目标地址——裸网络命名空间没有默认路由，指向不可路由地址即可覆盖 `ENETUNREACH` 发送失败路径。

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `LINKSTAY_TARGET` | `1.1.1.1` | 目标 IP 字面量（仅支持 IPv4/IPv6，不解析域名） |
| 检测间隔 | `-i, --interval` | `LINKSTAY_INTERVAL` | `10s` | 两次 ping 之间的间隔；纯数字按秒，支持 `s`/`ms` 后缀 |
| 失败阈值 | `-n, --threshold` | `LINKSTAY_THRESHOLD` | `5` | 连续失败次数触发关机 |
| 超时时间 | `-w, --timeout` | `LINKSTAY_TIMEOUT` | `3000ms` | 单次 ping 等待回包的超时；纯数字按毫秒，支持 `s`/`ms` 后缀；必须小于 `--interval` |
| 是否关机 | `-p, --poweroff` | `LINKSTAY_POWEROFF` | `false` | `true` 实际执行关机，`false` 仅模拟；接受 `true`/`false`（大小写不敏感） |
| 日志级别 | `-l, --log-level` | `LINKSTAY_LOG_LEVEL` | `info` | 取值为 `silent` / `error` / `warn` / `info` / `debug` |
| systemd 集成 | `-s, --systemd` | `LINKSTAY_SYSTEMD` | `true` | 启用 `sd_notify`、watchdog 与状态通知；接受 `true`/`false`（大小写不敏感）；省略参数时等价于启用，禁用建议写 `--systemd=false` 或 `-sfalse` |
| 版本信息 | `-v, --version` | — | — | 打印版本后退出（退出码 0） |
| 帮助信息 | `-h, --help` | — | — | 打印用法与全部选项后退出（退出码 0） |

优先级：CLI 参数 > 环境变量 > 编译期默认值。`--interval` 与 `--timeout` 均接受可选的 `s`/`ms` 后缀（仅小写，如 `-i 500ms`、`-w 1s`），内部统一以毫秒处理，唯一跨参数约束是 `timeout_ms < interval_ms`；日志与 `--help` 的时长统一由 `ls_format_duration()` 格式化（整秒显示为 `10s`，否则显示为 `1500ms`）。

短选项：必选参数既支持分开写法（如 `-i 5`），也支持粘连写法（如 `-i5`）；`-p`/`-s` 的可选布尔参数建议写成 `-pfalse`/`-sfalse`，或使用 `--poweroff=false`/`--systemd=false`。

## 探测与失败判定

每个周期只进行一次探测，连续失败达到 `--threshold` 后触发关机动作。以下两种情况都算一次失败：

- **回包超时**：请求已发出，但在 `--timeout` 内没有收到匹配的回包
- **发送被拒**：内核直接拒绝发送，属于路由/链路/缓冲类问题，例如接口断开导致的 `ENETUNREACH`、`ENETDOWN`、`ENOBUFS`

第二类失败同样计入连续失败计数，因此断网导致路由消失时监控仍会按阈值走完关机流程，而不是直接退出被 systemd 反复重启。只有套接字本身的错误（权限不足、参数非法、短写等）才会以退出码 1 终止进程。

## 关机行为说明

是否正式关机由布尔开关 `--poweroff` / `LINKSTAY_POWEROFF` 控制：

### `--poweroff=false`（默认）
达到阈值后模拟关机流程，但不实际执行关机命令；**模拟动作完成后进程会退出**，以保持与真实关机路径一致。

### `--poweroff=true`
达到阈值后立即执行真正关机。
关机后端仅有一个：统一调用 `systemctl --no-block poweroff`。
该模式要求主机存在可用的 systemd 环境（`/usr/bin/systemctl` 与 `/run/systemd/system`）。

## 日志时间戳行为

时间戳行为为**派生行为**：

- `--systemd=true`：日志进入 journald，linkstay 自动**关闭**前缀时间戳（避免与 journal 时间字段重复）
- `--systemd=false`：linkstay 自动**开启**时间戳，便于前台运行、重定向文件和手工排障

## 日志输出

每行为 `[时间戳] [级别] 消息`（时间戳是否出现取决于上一节），级别为 `ERROR` / `WARN` / `INFO` / `DEBUG`。常见行：

| 场景 | 级别 | 示例 |
|------|------|------|
| 启动横幅 | INFO | `linkstay 1.0 monitoring 1.1.1.1 \| interval 10s, timeout 3s, threshold 5 (detection ~50s), poweroff false (dry-run)` |
| 探测成功 | DEBUG | `Reply from 1.1.1.1: seq=7 time=12.34ms avg=13.02ms ok=7/7 (100.0%)` |
| 探测失败 | WARN | `Probe to 1.1.1.1 failed: no reply within 3s (failure 2 of 5)` |
| 统计信息 | INFO | `Statistics: uptime 1h2m3s \| 42 pings, 42 OK, 0 failed (100.00% success) \| latency min 0.004ms, avg 12.34ms, max 88.10ms` |
| 阈值动作 | WARN | `Failure threshold reached; poweroff is disabled (dry-run)` 后接 `[DRY-RUN] Would run /usr/bin/systemctl --no-block poweroff` |

显示细节：

- **延迟自适应精度**：小于 1ms 保留三位小数（如 `0.004ms`），1–100ms 两位，100ms 及以上一位；因此 loopback 这类亚毫秒链路不会再显示成 `0.00ms`
- **运行时长**以 `1h2m3s` / `3d2h1m5s` 形式显示，而不是累计秒数
- **单复数正确**：`1 ping` / `2 pings`
- `--poweroff=false` 时启动横幅与 `systemd` 状态里会标注 `(dry-run)`
- 探测失败原因使用系统错误文本（如 `Network is unreachable`）或超时描述（`no reply within 3s`），不再出现 `failed: Failed to ...` 这类重复措辞

## 信号处理

| 信号 | 行为 |
|------|------|
| `SIGTERM` | 优雅停止，输出最终统计后退出 |
| `SIGINT` | 同 `SIGTERM` |
| `SIGUSR1` | 立即输出当前统计信息（成功率、平均延迟、运行时间），不中断监控 |

## 排障示例

### `Timeout (...) must be smaller than interval (...)`

`--interval` 与 `--timeout` 内部均以毫秒处理，二者必须满足 `timeout_ms < interval_ms`：

```bash
# 错误：5000 ms 并不小于 5 s = 5000 ms
./bin/linkstay --target 1.1.1.1 --interval 5 --timeout 5000

# 正确：3000 ms 小于 5000 ms；也可统一用后缀避免心算
./bin/linkstay --target 1.1.1.1 --interval 5 --timeout 3000
./bin/linkstay --target 1.1.1.1 --interval 5s --timeout 3s
```

### `Operation not permitted (root or CAP_NET_RAW required)`

linkstay 使用 raw socket 发送 ICMP，因此需要 `root` 权限或 `CAP_NET_RAW`：

```bash
sudo ./bin/linkstay --target 1.1.1.1
```

若以 systemd 运行，请确认 unit 文件中的 `CapabilityBoundingSet=CAP_NET_RAW`、`RestrictAddressFamilies=AF_UNIX ...` 等设置未被额外覆盖。

## systemd 服务单元

`systemd/linkstay.service` 启用了核心沙箱隔离：

### 进程能力

| 能力 | 用途 |
|------|------|
| `CAP_NET_RAW` | ICMP raw socket |

### 安全隔离

| 指令 | 用途 |
|------|------|
| `NoNewPrivileges=true` | 禁止提权 |
| `ProtectSystem=strict` | 只读挂载 /usr、/boot、/etc |
| `RestrictAddressFamilies` | 仅允许 AF_UNIX、AF_INET、AF_INET6（无 DNS/netlink 需求，不含 AF_NETLINK） |
| `SystemCallFilter` | 白名单 @system-service @network-io @process |

### 资源限制

`MemoryMax=50M`、`TasksMax=10`、`OOMScoreAdjust=-100`（防止被 OOM killer 杀死）

## 项目结构

```
Makefile                # 构建、release、test、unit-test、lint、shellcheck、check 等入口
README.md               # 使用说明（面向用户的行为契约）
.gitignore              # 忽略构建产物、编辑器状态与本地 AI 工具目录
src/                    # 头文件与实现同目录，flat layout
├── base.h / base.c     # 无状态基础层：错误类型、单调时钟、饱和运算、时长格式化
├── log.h / log.c       # 分级日志：项目内唯一的 stderr 输出点
├── addr.h / addr.c     # IPv4/IPv6 字面量解析（配置校验与传输层共用同一实现）
├── opts.h / opts.c     # 配置解析：默认值 → CLI → 环境变量 → 校验
├── loop.h / loop.c     # poll(2) 事件循环、绝对 deadline 定时器、signalfd
├── icmp.h / icmp.c     # ICMP raw socket、BPF 过滤、校验和、回包匹配
├── notify.h / notify.c # systemd sd_notify 后端：READY/STATUS/WATCHDOG/STOPPING
├── stats.h / stats.c   # 累计计数与延迟聚合：成功率、平均/最小/最大、运行时长
├── app.h / app.c       # 组装层：probe 状态机、阈值动作、启动/退出横幅
└── main.c              # 薄入口：opts → app → loop
systemd/
└── linkstay.service  # systemd unit 示例文件
tests/
├── unit.c            # 单元断言（base / addr / log / loop / stats / opts）
├── run_tests.sh      # shell 套件入口：加载辅助模块、执行并汇总
├── lib.sh            # 断言、进程控制、NOTIFY 接收器辅助
├── cli.sh            # 参数/环境变量解析、优先级、调试输出
├── live.sh           # 实网 ICMP、信号、阈值动作
└── notify.sh         # NOTIFY_SOCKET 协议用例（由 live.sh 加载）
.github/
├── copilot-instructions.md  # 工程契约：不变量、模块边界、验证流程
└── workflows/ci.yml         # CI：安装检查工具并执行 make check
```

`bin/`（默认构建）与 `bin-sanitize/`（sanitizer 构建）是 `make` 生成的产物，已被 `.gitignore` 忽略，可随时用 `make clean` 删除。

代码以 `-std=c23 -pedantic-errors` 构建，`base.h` 中有 `__STDC_VERSION__` 守卫，使用 C17 及更早标准会直接编译失败。

工程约定（不变量、命名边界、模块依赖方向、测试与 lint 基线）集中记录在 `.github/copilot-instructions.md`，修改代码前请先阅读该文件。
