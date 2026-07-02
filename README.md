# linkstay - 高性能网络监控工具

[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**linkstay** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket + BPF 内核过滤，无需依赖系统 `ping` 命令
- **可选的真实关机**：通过布尔开关 `--poweroff` 决定是否正式关机；`false` 仅模拟，`true` 达到阈值后立即执行关机
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知；watchdog 随 systemd 自动启用
- **高性能**：单一二进制文件 ≈ 48 KB，内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：编译期 Full RELRO、PIE、Stack Canary、NX、FORTIFY\_SOURCE；运行期 systemd 沙箱

## 快速开始

### 1. 构建

```bash
make          # 构建 bin/linkstay
make release  # 构建后 strip
make test     # 运行功能测试
```

### 2. 运行

```bash
# 查看帮助
./bin/linkstay --help

# 前台调试运行（不实际关机，仅模拟）
sudo ./bin/linkstay --target 1.1.1.1 --interval 10 --timeout 2000 \
     --threshold 3 --log-level debug

# 短选项示例：必选参数既可写成 "-i 5"，也可写成 "-i5"
# 带可选参数的 -p / -s 建议写成 -p0/-s0，或使用 --poweroff=0/--systemd=0
sudo ./bin/linkstay -t 1.1.1.1 -i 5 -w 1000 -n 3 -l debug -s0
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
| `make` | 构建 `bin/linkstay` |
| `make release` | 构建后 strip |
| `make test` | 运行 `tests/run_tests.sh` |
| `make lint` | cppcheck + clang-tidy |
| `make install` | 安装二进制到 `$(PREFIX)/bin/linkstay`，默认 `PREFIX=/usr/local` |
| `make install-systemd` | 安装二进制和 `systemd/linkstay.service` |
| `make uninstall` | 删除已安装的二进制和 systemd unit |
| `make clean` | 清理 `bin/` |

安装目标支持 staging，例如：

```bash
make install-systemd DESTDIR=/tmp/linkstay-stage PREFIX=/usr/local
```

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `LINKSTAY_TARGET` | `1.1.1.1` | 目标 IP 字面量（仅支持 IPv4/IPv6，不解析域名） |
| 检测间隔 | `-i, --interval` | `LINKSTAY_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold` | `LINKSTAY_THRESHOLD` | `5` | 连续失败次数触发关机 |
| 超时时间 | `-w, --timeout` | `LINKSTAY_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时，必须小于 interval |
| 是否关机 | `-p, --poweroff` | `LINKSTAY_POWEROFF` | `false` | `true` 实际执行关机，`false` 仅模拟；接受 `true/false/1/0/yes/no/on/off` |
| 日志级别 | `-l, --log-level` | `LINKSTAY_LOG_LEVEL` | `info` | 规范值为 `silent` / `error` / `warn` / `info` / `debug`；兼容别名 `none=silent`、`warning=warn` |
| systemd 集成 | `-s, --systemd` | `LINKSTAY_SYSTEMD` | `true` | 启用 `sd_notify`、watchdog 与状态通知；接受 `true/false/1/0/yes/no/on/off`；省略参数时等价于启用，禁用建议写 `--systemd=0`、`--systemd=false`、`-s0` 或 `-sfalse` |

优先级规则：CLI 参数 > 环境变量 > 编译期默认值。

短选项格式说明：

- 必选参数既支持分开写法（如 `-i 5`），也支持粘连写法（如 `-i5`）
- `-p`、`-s` 对应可选参数；不带值时等价于启用，若要显式关闭推荐使用 `--poweroff=0`/`-p0`、`--systemd=0`/`-s0`

## 关机行为说明

是否正式关机由布尔开关 `--poweroff` / `LINKSTAY_POWEROFF` 控制：

### `--poweroff=false`（默认）
达到阈值后模拟关机流程，但不实际执行关机命令；**模拟动作完成后进程会退出**，以保持与真实关机路径一致。

### `--poweroff=true`
达到阈值后立即执行真正关机。
统一调用 `systemctl --no-block poweroff`，不再保留非 systemd 的关机后端。
该模式要求主机存在可用的 systemd 环境（`/usr/bin/systemctl` 与 `/run/systemd/system`）。

## 日志时间戳行为

时间戳行为为**派生行为**：

- `--systemd=true`：日志进入 journald，linkstay 自动**关闭**前缀时间戳（避免与 journal 时间字段重复）
- `--systemd=false`：linkstay 自动**开启**时间戳，便于前台运行、重定向文件和手工排障

## 信号处理

| 信号 | 行为 |
|------|------|
| `SIGTERM` | 优雅停止，输出最终统计后退出 |
| `SIGINT` | 同 `SIGTERM` |
| `SIGUSR1` | 立即输出当前统计信息（成功率、平均延迟、运行时间），不中断监控 |

## 排障示例

### `Timeout (...) must be smaller than interval (...)`

`--timeout` 的单位是毫秒，`--interval` 的单位是秒，二者必须满足：

```text
timeout_ms < interval_sec * 1000
```

例如：

```bash
# 错误：5000 ms 并不小于 5 s = 5000 ms
./bin/linkstay --target 1.1.1.1 --interval 5 --timeout 5000

# 正确：3000 ms 小于 5 s = 5000 ms
./bin/linkstay --target 1.1.1.1 --interval 5 --timeout 3000
```

### `Operation not permitted (require root or CAP_NET_RAW)`

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
| `PrivateTmp=true` | 隔离 /tmp |
| `ProtectSystem=strict` | 只读挂载 /usr、/boot、/etc |
| `ProtectHome=true` | 隐藏 /home |
| `UMask=0077` | 收紧新建文件权限 |
| `ProtectKernelTunables` / `ProtectKernelModules` / `ProtectKernelLogs` | 禁止改写内核参数、加载模块、读取内核日志 |
| `ProtectControlGroups` / `ProtectClock` / `ProtectHostname` | 保护 cgroup、系统时钟与主机名 |
| `RestrictRealtime` / `RestrictSUIDSGID` / `RestrictNamespaces` / `LockPersonality` | 禁用实时调度、SUID/SGID、命名空间与 personality 切换 |
| `RestrictAddressFamilies` | 仅允许 AF_UNIX、AF_INET、AF_INET6（无 DNS/netlink 需求，不含 AF_NETLINK） |
| `SystemCallFilter` | 白名单 @system-service @network-io @process |

### 资源限制

`MemoryMax=50M`、`TasksMax=10`、`OOMScoreAdjust=-100`（防止被 OOM killer 杀死）

## 项目结构

```
Makefile                # 构建、release、lint、clean 入口
README.md               # 使用说明与维护约定
include/                # 公共头文件
├── common.h           # 基础层：宏、常量、单调时钟声明、静态断言
├── logger.h           # 分级日志接口
├── metrics.h          # ping 统计指标聚合
├── config.h           # 配置类型、解析/校验接口
├── icmp.h             # ICMP raw socket、BPF 过滤、回包匹配接口
├── shutdown.h         # 关机执行接口
├── systemd.h          # systemd notify socket 集成接口
└── monitor.h          # 聚合各模块并定义共享运行时对象 linkstay_ctx_t
src/                    # 实现
├── common.c           # 单调时钟实现
├── logger.c           # 分级日志、时间戳格式化
├── metrics.c          # ping 统计指标聚合
├── config.c           # 配置默认值、CLI/环境变量解析、usage/version、校验
├── icmp.c             # ICMP raw socket、BPF 过滤、校验和、回包匹配
├── shutdown.c         # 关机执行（posix_spawn + 启动观测）
├── systemd.c          # systemd notify socket 集成
├── monitor.c          # reactor 主循环、定时器状态机、shutdown FSM、编排
└── main.c             # 入口
systemd/
└── linkstay.service  # systemd unit 示例文件
.github/
└── copilot-instructions.md  # AI 协作与代码维护约定
```

模块按层次组织：`include/common.h` 位于底层，上层模块各自拥有独立头文件，`include/monitor.h` 聚合所有模块并定义共享运行时对象 `linkstay_ctx_t`。头文件统一存放于 `include/`，实现统一存放于 `src/`，服务示例统一放在 `systemd/`，便于查阅与维护。
