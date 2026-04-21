# LinkStay - 高性能网络监控工具

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**LinkStay** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket + BPF 内核过滤，无需依赖系统 `ping` 命令
- **灵活的关机策略**：支持 `dry-run`、`true-off`、`log-only` 三种模式，`--delay` 独立控制程序内倒计时
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知；watchdog 随 systemd 自动启用
- **高性能**：单一二进制文件 ≈ 48 KB，内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：编译期 Full RELRO、PIE、Stack Canary、NX、FORTIFY\_SOURCE；运行期 systemd 沙箱

## 快速开始

### 1. 构建

```bash
make          # 构建 bin/LinkStay
make release  # 构建后 strip
```

### 2. 运行

```bash
# 查看帮助
./bin/LinkStay --help

# 前台调试运行（干跑模式，不实际关机）
sudo ./bin/LinkStay --target 1.1.1.1 --interval 10 --timeout 2000 \
     --threshold 3 --mode dry-run --log-level debug

# 短选项示例：必选参数既可写成 "-i 5"，也可写成 "-i5"
# 只有带可选参数的 -s 建议写成 -s0/-sfalse，或直接使用 --systemd=0/--systemd=false
sudo ./bin/LinkStay -t 1.1.1.1 -i 5 -w 1000 -n 3 -m dry-run -d 0 -l debug -s0
```

### 3. 可选：手动注册 systemd 服务

```bash
sudo cp bin/LinkStay /usr/local/bin/LinkStay
sudo cp systemd/LinkStay.service /etc/systemd/system/LinkStay.service
sudo systemctl daemon-reload
sudo systemctl enable --now LinkStay
```

仓库内附带的示例 unit 默认使用 `LINKSTAY_MODE=log-only`，这样服务会持续监控；确认行为后再显式切换到 `true-off`。

服务启动后可通过 drop-in 覆盖 `Environment=` 行来修改配置，无需改动原始 unit 文件：

```bash
sudo systemctl edit LinkStay
# 在弹出的编辑器里写入：
# [Service]
# Environment="LINKSTAY_TARGET=192.168.1.1"
# Environment="LINKSTAY_MODE=true-off"

sudo systemctl daemon-reload
sudo systemctl restart LinkStay
```

查看实时日志：

```bash
journalctl -fu LinkStay
```

### 4. Make 目标

| 目标 | 说明 |
|------|------|
| `make` | 构建 `bin/LinkStay` |
| `make release` | 构建后 strip |
| `make lint` | cppcheck + clang-tidy |
| `make clean` | 清理 `bin/` |

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `LINKSTAY_TARGET` | `1.1.1.1` | 目标 IP 字面量（仅支持 IPv4/IPv6，不解析域名） |
| 检测间隔 | `-i, --interval` | `LINKSTAY_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold, --fail-threshold` | `LINKSTAY_THRESHOLD` | `5` | 连续失败次数触发关机；兼容读取 `LINKSTAY_FAIL_THRESHOLD` |
| 超时时间 | `-w, --timeout` | `LINKSTAY_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时，必须小于 interval |
| 关机模式 | `-m, --mode` | `LINKSTAY_MODE` | `dry-run` | `dry-run` / `true-off` / `log-only` |
| 倒计时分钟 | `-d, --delay` | `LINKSTAY_DELAY` | `0` | 程序内关机倒计时（分钟），范围 `0..525600`；`0` 表示立即执行；对 `log-only` 无效 |
| 日志级别 | `-l, --log-level` | `LINKSTAY_LOG_LEVEL` | `info` | 规范值为 `silent` / `error` / `warn` / `info` / `debug`；兼容别名 `none=silent`、`warning=warn` |
| systemd 集成 | `-s, --systemd` | `LINKSTAY_SYSTEMD` | `true` | 启用 `sd_notify`、watchdog 与状态通知；接受 `true/false/1/0/yes/no/on/off`；省略参数时等价于启用，禁用建议写 `--systemd=0`、`--systemd=false`、`-s0` 或 `-sfalse` |

优先级规则：CLI 参数 > 环境变量 > 编译期默认值。

短选项格式说明：

- 必选参数既支持分开写法（如 `-i 5`、`-d 3`），也支持粘连写法（如 `-i5`、`-d3`）
- `-s` 对应可选参数；若要显式关闭，推荐使用 `--systemd=0`、`--systemd=false`、`-s0` 或 `-sfalse`
- `LINKSTAY_THRESHOLD` 兼容别名 `LINKSTAY_FAIL_THRESHOLD`；若两者同时设置且值不同，程序会拒绝启动以避免歧义

## 关机模式说明

### `dry-run`
达到阈值后模拟关机流程，但不实际执行关机命令；**模拟动作完成后进程会退出**，以保持与真实关机路径一致。  
`--delay > 0` 时先启动程序内倒计时；在倒计时结束前网络恢复可取消本次计划动作。

### `true-off`
达到阈值后执行真正关机。  
统一调用 `systemctl --no-block poweroff`，不再保留非 systemd 的关机后端。  
该模式要求主机存在可用的 systemd 环境（`/usr/bin/systemctl` 与 `/run/systemd/system`）。  
`--delay > 0` 时同样先进行程序内倒计时，届时立即执行关机。

### `log-only`
阈值触发时只记录警告日志并**重置失败计数器**，进程持续监控，永不执行关机。  
适用于将 LinkStay 作为纯网络探针或配合外部告警系统使用的场景；仓库内示例 `systemd/LinkStay.service` 默认就使用该模式。  
由于达到阈值后会立即记录并重置计数器，`log-only` 模式下 `--delay` 没有语义，因此配置为非 `0` 时会被拒绝。

## 日志时间戳行为

时间戳行为为**派生行为**：

- `--systemd=true`：日志进入 journald，LinkStay 自动**关闭**前缀时间戳（避免与 journal 时间字段重复）
- `--systemd=false`：LinkStay 自动**开启**时间戳，便于前台运行、重定向文件和手工排障

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
./bin/LinkStay --target 1.1.1.1 --interval 5 --timeout 5000

# 正确：3000 ms 小于 5 s = 5000 ms
./bin/LinkStay --target 1.1.1.1 --interval 5 --timeout 3000
```

### `Operation not permitted (require root or CAP_NET_RAW)`

LinkStay 使用 raw socket 发送 ICMP，因此需要 `root` 权限或 `CAP_NET_RAW`：

```bash
sudo ./bin/LinkStay --target 1.1.1.1 --mode dry-run
```

若以 systemd 运行，请确认 unit 文件中的 `CapabilityBoundingSet=CAP_NET_RAW`、`RestrictAddressFamilies=AF_UNIX ...` 等设置未被额外覆盖。

## systemd 服务单元

`systemd/LinkStay.service` 启用了核心沙箱隔离：

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
| `RestrictAddressFamilies` | 仅允许 AF_UNIX、AF_INET、AF_INET6、AF_NETLINK |
| `SystemCallFilter` | 白名单 @system-service @network-io @process |

### 资源限制

`MemoryMax=50M`、`TasksMax=10`、`OOMScoreAdjust=-100`（防止被 OOM killer 杀死）

## 项目结构

```
src/
├── linkstay.h          # 公共类型与 API 声明
├── config.c           # 配置默认值、CLI/环境变量解析、usage/version、校验
├── monitor.c          # 监控主循环（metrics、状态机、shutdown FSM、reactor）
├── icmp.c             # ICMP raw socket、BPF 过滤、校验和
├── logger.c           # 日志、单调时钟、时间戳
├── shutdown.c         # 关机执行（posix_spawn）
├── systemd.c          # systemd notify socket 集成
├── monitor.h          # monitor 模块公开 API
└── main.c             # 入口
systemd/
└── LinkStay.service  # systemd unit 文件
```

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。
