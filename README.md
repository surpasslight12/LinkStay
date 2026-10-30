# linkstay — systemd 网络监控与自动关机服务

[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-native-green.svg)](https://systemd.io/)

**linkstay** 是一个 C23 编写的 Linux systemd 服务。它通过 raw socket + BPF
周期性探测目标 IP，连续失败达到阈值后调用 `systemctl poweroff`。项目没有
CLI、前台调试模式或 daemonize 逻辑：systemd 是唯一支持的运行方式。

## 快速导航

- [核心特性](#核心特性)
- [构建与安装](#构建与安装)
- [配置](#配置)
- [服务管理](#服务管理)
- [运行要求](#运行要求)
- [延迟启动](#延迟启动)
- [探测与失败判定](#探测与失败判定)
- [关机行为](#关机行为)
- [日志与信号](#日志与信号)
- [systemd 沙箱](#systemd-沙箱)
- [测试](#测试)
- [项目结构](#项目结构)
- [排障](#排障)
- [工程契约](#工程契约)

## 核心特性

- **systemd 原生**：`Type=notify`，实现 `READY=1`、`STATUS=`、
  `WATCHDOG=1`、`STOPPING=1`；由 `WatchdogSec` 驱动。
- **无命令行**：所有配置来自 unit 的 `Environment=` 与
  `/etc/linkstay.conf`，不存在 CLI 参数解析层。
- **原生 ICMP**：raw socket + 内核 BPF 过滤，不依赖系统 `ping`。
- **延迟启动**：首次探测前可等待一段时间，unit 默认 30s，便于等待链路稳定。
- **真实关机**：unit 默认 `LINKSTAY_POWEROFF=true`；改为 `false` 可走
  dry-run。
- **小体积**：LTO 构建，stripped 二进制约 34 KiB。
- **沙箱**：systemd unit 限制 capability、文件系统、地址族与系统调用。

## 构建与安装

前置条件：`make`、支持 C23 的 GCC/Clang。运行完整检查还需要
`cppcheck`、`clang-tidy`、`shellcheck`、`python3`。

```bash
make               # 构建 bin/linkstay
make check         # 单元 + shell + lint + shellcheck + sanitizer

sudo make install  # 安装二进制、unit 与 /etc/linkstay.conf
sudo systemctl daemon-reload
sudo systemctl enable --now linkstay
```

默认安装路径：

| 内容 | 路径 |
|------|------|
| 二进制 | `/usr/local/bin/linkstay` |
| systemd unit | `/etc/systemd/system/linkstay.service` |
| 配置 | `/etc/linkstay.conf` |

`make install` 会用最终的 `BINDIR` 和 `CONFIG_FILE` 替换 unit 中的占位
路径；支持 `DESTDIR` staging。`PREFIX` 默认 `/usr/local`，unit 默认的
`ExecStart` 与之一致。重复安装不会覆盖已存在的 `/etc/linkstay.conf`。

`make uninstall` 删除二进制和 unit，但**保留** `/etc/linkstay.conf`，避免
删除管理员修改过的配置。

安装后确认服务状态：

```bash
systemctl status --no-pager linkstay
journalctl -u linkstay -b --no-pager
```

## 配置

unit 先设置一组默认 `Environment=`，随后读取可选的
`/etc/linkstay.conf`（`EnvironmentFile=-/etc/linkstay.conf`）。后者覆盖前者。

| 环境变量 | 默认值（代码） | unit 默认 | 说明 |
|----------|----------------|-----------|------|
| `LINKSTAY_TARGET` | `1.1.1.1` | `1.1.1.1` | 目标 IP 字面量，不解析 DNS |
| `LINKSTAY_INTERVAL` | `10s` | `10s` | 探测间隔，必须带 `s`/`ms` 后缀 |
| `LINKSTAY_THRESHOLD` | `5` | `5` | 连续失败次数 |
| `LINKSTAY_TIMEOUT` | `3000ms` | `3000ms` | 回包超时，必须小于 interval |
| `LINKSTAY_START_DELAY` | `0s` | `30s` | 首次探测前等待；`0s` 表示立即探测 |
| `LINKSTAY_POWEROFF` | `false` | `true` | `true` 真实关机，`false` dry-run |
| `LINKSTAY_LOG_LEVEL` | `info` | `info` | `silent`/`error`/`warn`/`info`/`debug` |

“代码默认”用于测试或直接启动；“unit 默认”是安装后的生产配置。通过 systemd
运行时以 unit 与 `/etc/linkstay.conf` 为准。因此默认安装会真实关机，并等待
30s 再开始首次探测。

时长只接受小写 `s` 或 `ms` 后缀；裸数字、前导 `+` 与大小写变体都会被拒绝。
`LINKSTAY_START_DELAY` 额外接受 `0s`/`0ms`。布尔值只接受大小写不敏感的
`true`/`false`。

编辑配置后重启服务：

```bash
sudoedit /etc/linkstay.conf
sudo systemctl restart linkstay
```

也可以使用 drop-in：

```bash
sudo systemctl edit linkstay
# [Service]
# Environment="LINKSTAY_TARGET=192.168.1.1"
# Environment="LINKSTAY_POWEROFF=false"
sudo systemctl daemon-reload
sudo systemctl restart linkstay
```

## 服务管理

```bash
sudo systemctl daemon-reload          # unit 或 drop-in 变更后
sudo systemctl restart linkstay
sudo systemctl status --no-pager linkstay
sudo systemctl disable --now linkstay # 停止并取消开机启动
journalctl -fu linkstay
```

## 运行要求

- **必须由 systemd 启动**：`NOTIFY_SOCKET` 缺失或不可连接时，进程立即以
  退出码 1 失败；没有“脱离 systemd 继续运行”的兼容模式。
- **raw ICMP**：需要 root 或 `CAP_NET_RAW`；unit 以 root 运行，并通过
  `CapabilityBoundingSet=CAP_NET_RAW` 移除其余 capability。
- **真实关机**：`LINKSTAY_POWEROFF=true` 还要求
  `/usr/bin/systemctl` 与 `/run/systemd/system` 可用。
- **测试**：shell 套件需要 `python3` 创建 NOTIFY_SOCKET 接收器；实网用例
  需要 root/CAP_NET_RAW，或使用用户+网络命名空间。

## 延迟启动

linkstay 在初始化完成后立即发送 `READY=1`，因此 systemd 启动超时不等同于首次
探测延迟。首次探测由 `LINKSTAY_START_DELAY` 控制：

- unit 默认 `30s`，适合 `network-online.target` 之后等待链路稳定；
- 设置为 `0s` 可立即探测；
- 等待期间 watchdog 心跳照常发送，服务不会被 `WatchdogSec` 误杀；
- 启动日志与 `STATUS=` 会显示 `first probe in ...` / `Waiting ...`。

```ini
# /etc/linkstay.conf
LINKSTAY_START_DELAY=0s
```

## 探测与失败判定

每个周期只进行一次探测，连续失败达到阈值后触发关机动作。以下情况都算一次
失败：

- **回包超时**：请求已发出，但在 `LINKSTAY_TIMEOUT` 内没有匹配回包。
- **发送被拒**：内核直接拒绝发送，例如 `ENETUNREACH`、`ENETDOWN`、
  `ENOBUFS`。这类失败同样计入阈值，避免断网时服务被 systemd 反复重启。

socket 本身不可用（权限、参数、短写等）才是致命错误，进程以退出码 1 退出，
由 `Restart=on-failure` 交给 systemd 处理。

## 关机行为

- `LINKSTAY_POWEROFF=false`：达到阈值后记录 dry-run 日志并退出 0；不执行
  关机，也不触发自动重启。
- `LINKSTAY_POWEROFF=true`：unit 默认行为，调用
  `/usr/bin/systemctl --no-block poweroff`。若调用失败，进程记录错误并继续
  监控，连续失败计数保持不变。

## 日志与信号

服务不读取 stdin，stdout 被丢弃（unit 设置 `StandardInput=null`、
`StandardOutput=null`），日志只写 stderr，并由 journald 收集。每行只有
`[LEVEL] message`，**不包含时间戳**，避免与 journal 字段重复。

```bash
journalctl -fu linkstay
```

| 信号 | 行为 |
|------|------|
| `SIGTERM` | 优雅停止，发送 `STOPPING=1`，输出最终统计后退出 |
| `SIGINT` | 同 `SIGTERM` |
| `SIGUSR1` | 立即输出当前统计，不中断监控 |

## systemd 沙箱

`systemd/linkstay.service` 启用了：

- `Type=notify`、`NotifyAccess=main`、`WatchdogSec=30`；
- `User=root` + `CapabilityBoundingSet=CAP_NET_RAW`；
- `ProtectSystem=strict`、`ProtectHome=true`、内核/时钟/主机名保护；
- `PrivateTmp=true`、`UMask=0077`、`MemoryDenyWriteExecute=true`；
- `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`（不需要 DNS/netlink）；
- `SystemCallFilter=@system-service @network-io @process`；
- `MemoryMax=50M`、`TasksMax=10`、`OOMScoreAdjust=-100`。

unit 通过 `EnvironmentFile=-/etc/linkstay.conf` 读取配置；如修改 unit 本身，
记得 `systemctl daemon-reload`。

## 测试

```bash
make unit-test      # 147 项单元断言
make test           # 单元 + shell（43 项通过、1 项实网跳过）
make check          # test + lint + shellcheck + sanitize-test
```

实网套件需要 root/CAP_NET_RAW；在用户+网络命名空间中运行时完整套件为
69 项通过。`make check` 需要 `cppcheck`、`clang-tidy`、`shellcheck` 与
`python3`。

实网测试：

```bash
unshare -Urn --map-root-user bash -c \
  'ip link set lo up; LINKSTAY_TEST_LIVE=1 LINKSTAY_TEST_UNREACHABLE_TARGET=203.0.113.1 make test'
```

测试套件会在启动时创建一个全局 NOTIFY_SOCKET 接收器，因此每个服务实例都
满足“必须由 systemd 启动”的约束；协议用例会为单次运行创建独立接收器。

## 项目结构

```
Makefile                # 构建、测试、lint、sanitize、安装
README.md               # 使用说明与行为契约
.gitignore              # 构建产物与本地状态忽略规则
.github/
└── copilot-instructions.md # 工程契约
src/
├── base.h / base.c     # 错误类型、单调时钟、饱和运算、时长格式化
├── log.h / log.c       # journald 友好的分级 stderr 日志
├── addr.h / addr.c     # IPv4/IPv6 字面量解析
├── config.h / config.c # 默认值 + LINKSTAY_* 环境变量 + 校验
├── loop.h / loop.c     # poll(2)、绝对 deadline 定时器、signalfd
├── icmp.h / icmp.c     # raw ICMP、BPF、校验和、回包匹配
├── notify.h / notify.c # 必需的 sd_notify 通道
├── stats.h / stats.c   # 累计计数与延迟聚合
└── app.c               # 进程入口与组装：probe、阈值、启动/退出
systemd/
├── linkstay.service    # systemd unit（安装时替换路径占位符）
└── linkstay.conf       # /etc/linkstay.conf 示例
tests/
├── unit.c              # 单元断言
├── run_tests.sh        # shell 套件入口与全局 notify 接收器
├── lib.sh              # 断言、进程、notify 辅助
├── config.sh           # 环境配置与 systemd-only 启动
├── live.sh             # 实网 ICMP、信号、阈值
└── notify.sh           # READY/STATUS/WATCHDOG/STOPPING
```

## 排障

### `NOTIFY_SOCKET is not set`

说明进程不是由 systemd 启动的。使用：

```bash
sudo systemctl restart linkstay
journalctl -u linkstay -n 50
```

### `poweroff requires a systemd host`

`LINKSTAY_POWEROFF=true` 时要求 `/usr/bin/systemctl` 和
`/run/systemd/system`。若只想测试监控，改为 `LINKSTAY_POWEROFF=false`。

### `Operation not permitted (root or CAP_NET_RAW required)`

确认 unit 的 `CapabilityBoundingSet=CAP_NET_RAW` 未被覆盖；手工运行二进制时
需要 root。

### `Timeout (...) must be smaller than interval (...)`

调整 `/etc/linkstay.conf` 中的 `LINKSTAY_TIMEOUT`/`LINKSTAY_INTERVAL`，确保
前者严格小于后者。

## 工程契约

不变量、模块边界、命名规则、测试与 lint 基线记录在
[.github/copilot-instructions.md](.github/copilot-instructions.md)。修改代码前
请先阅读该文件。
