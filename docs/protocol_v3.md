# PM_Tiny 本地 IPC 协议（v3）

## 适用范围

Linux 版 `pm`、Android 版 `pm2`、`pm_tiny` 和 `pm_sdk` 使用 Unix Domain `SOCK_STREAM` 通信。协议 v3 是固定长度头部的二进制协议，不使用 JSON、HTTP 或换行分帧。Android 客户端只改变安装文件名以避开系统 `/system/bin/pm`，协议和 CMake target 仍与 Linux 客户端相同。

Linux daemon 使用 standalone Asio 调度 UDS、客户端 session 和子进程输出管道；CLI 日志等待和 Linux/Windows SDK 也复用 Asio 传输。协议编解码仍由 `protocol_v3` 独立完成，因此传输层不依赖一次读取对应一帧。

socket 地址由 `PM_TINY_SOCK_FILE` 或 `pm_tiny.yaml` 配置；`PM_TINY_UDS_ABSTRACT_NAMESPACE=1` 时使用 Linux abstract namespace。

## 帧格式

每帧由 16 字节头部和 payload 组成：

| 偏移 | 长度 | 字段 | 编码 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PMT3` |
| 4 | 1 | version | `3` |
| 5 | 1 | flags | 位标志 |
| 6 | 2 | message_type | 大端 `u16` |
| 8 | 4 | request_id | 大端 `u32` |
| 12 | 4 | payload_len | 大端 `u32` |
| 16 | payload_len | payload | 二进制数据 |

payload 最大 4 MiB。TCP/UDS 流可能产生半包或粘包，接收端必须按 `payload_len` 重组，不能依赖一次 `read()` 对应一帧。

整数使用大端序；字符串使用 `u32 大端长度 + UTF-8 字节`。长度必须非负、未溢出且不超过最大 payload。

flags：

- `0x01`：响应帧
- `0x02`：错误帧
- `0x04`：日志/数据流帧
- `0x08`：后续仍有分片

普通响应 payload 的开头固定为：

```text
i32 status_code
string message
```

成功码为 `0`，失败码为负数。响应必须回显请求的 `request_id`。

## 消息类型

| 类型 | 含义 |
| ---: | --- |
| `0x23` | ls |
| `0x24` | stop |
| `0x25` | start |
| `0x26` | save |
| `0x27` | delete |
| `0x28` | restart |
| `0x29` | version |
| `0x30` | show log |
| `0x31` | 应用 ready |
| `0x32` | 应用 tick |
| `0x33` | inspect |
| `0x34` | reload |
| `0x35` | quit |
| `0x36` | daemon info |

### `0x36` daemon info

`pm info`（Android 为 `pm2 info`）发送空 payload。成功响应在通用 `status_code` 和 `message`
后附带 `daemon_info_schema_version = 1` 及公共 daemon 快照，覆盖身份、运行模式、健康状态、最终配置、
IPC、日志、进程树和能力。该命令只读，不返回受管程序环境变量，也不替代 `pm inspect <name>`。

配置值的来源枚举为 `default`、`config_file`、`environment`、`command_line` 和 `derived`。
相对路径转换为最终路径时保留原始来源；由 home 补出的 PID、日志、程序配置、环境和 IPC 路径标记为
`derived`。数组、能力列表和来源映射均有数量上限；未知 schema、非法枚举、重复来源键、截断和尾随
payload 均作为协议错误拒绝。

`pm info --json` 的稳定顶层键为：

```text
schema_version, identity, runtime, config, ipc, logging, process_tree, capabilities
```

配置项以 `{value, source}` 表示，布尔、整数和数组保持原始 JSON 类型。旧 daemon 不认识 `0x36` 时
返回未知命令错误，不提供兼容层。

### `0x23` 进程列表

`0x23` 成功响应在通用的 `status_code` 和 `message` 后附带以下结构：

```text
i32 list_schema_version  // 当前为 5
i32 process_count
repeat process_count times:
  i64 pid                // 不可用时为 -1
  string name
  string cwd
  string executable
  i32 argument_count
  repeat argument_count times: string argument
  i32 restart_count
  i32 state_code
  i32 has_uptime         // 只能为 0 或 1
  i64 uptime_ms
  i32 has_rss            // 只能为 0 或 1
  i64 rss_kib
  i32 daemon             // 只能为 0 或 1
  i32 pty                // -1 不支持，0 关闭，1 开启
  i32 dependency_count
  repeat dependency_count times: string dependency
  i32 restart_pending                 // 只能为 0 或 1
  i64 restart_delay_remaining_ms
  i32 restart_attempts_in_window
  i32 restart_suppressed              // 只能为 0 或 1
  string restart_suppression_reason
  u64 generation
  i32 ready                            // 只能为 0 或 1
  i32 heartbeat_enabled                // 只能为 0 或 1
  i32 has_last_exit                    // 只能为 0 或 1
  string last_exit_reason              // exited/signaled/unknown；不可用时为空
  i32 last_exit_code                   // 退出码或信号编号
  string process_tree_backend          // cgroup/process_group/job_object
  i32 process_tree_degraded             // 只能为 0 或 1
  string process_tree_degradation_reason
  string config_source                  // file/runtime
  i32 log_degraded                      // 只能为 0 或 1
  u64 log_dropped_bytes
  string log_last_error
  i64 log_retry_remaining_ms
  i32 log_path_count
  repeat log_path_count times: string log_path
```

列表 schema 版本独立于帧协议版本。当前升级直接替换了旧的 `0x23` 字段布局和 Windows 文本响应，不提供兼容回退；部署时必须同步替换 daemon 与 CLI，新旧版本不能混用。

`pm graph`/`pm dag` 不增加新的消息类型，而是请求同一 `0x23` 列表快照并在客户端构建只读依赖图。
因此其文本、JSON schema v1 和 DOT 输出属于 CLI 接口，不属于 daemon 二进制协议。

### `0x33` inspect

Linux/Android 和 Windows 使用相同的二进制 inspect 响应，不传输平台自行拼接的文本。成功响应在
通用 `status_code` 和 `message` 后附带：

```text
i32 inspect_schema_version             // 当前为 2
prog_cfg                               // 与 start request 相同的公共配置字段
i64 pid
u64 generation
i32 state_code
i32 restart_count
i32 ready
i32 heartbeat_enabled
i32 has_last_tick_age
i64 last_tick_age_ms
i32 has_uptime
i64 uptime_ms
i32 has_rss
i64 rss_kib
i32 pty                                // -1 不支持，0 关闭，1 开启
i32 restart_pending
i64 restart_delay_remaining_ms
i32 restart_attempts_in_window
i32 restart_suppressed
string restart_suppression_reason
i32 has_last_exit
i32 last_exit_reason                   // 0 none, 1 exited, 2 signaled, 3 unknown
i32 last_exit_code
string process_tree_backend
i32 process_tree_degraded
string process_tree_degradation_reason
string config_source
i32 log_degraded
u64 log_dropped_bytes
string log_last_error
i64 log_retry_remaining_ms
i32 log_path_count
repeat log_path_count times: string log_path
```

availability 字段为 0 时，对应数值字段只是占位，不应解释为实际采样值。Windows 的 PTY 返回
`-1`，进程树后端返回 `job_object`；POSIX 可区分普通退出和 signal，Windows 返回 `exited` 及
`CreateProcessW` 进程退出码。CLI 在三个平台使用同一个 renderer，命令名称、字段顺序和字段语义一致。

### `0x25` start

start 使用共享 schema，不再传输待二次解析的命令字符串：

```text
i32 start_schema_version  // 当前为 2
i32 mode                  // 0 existing, 1 create
string name
prog_cfg                   // name/cwd/executable/args/完整公共配置
string[] inherited_env
i32 show_log
```

响应在通用 `status_code` 和 `message` 后附带：

```text
i32 start_schema_version
i32 result                // 0 started, 1 waiting, 2 blocked
i64 pid
string state
string[] blocked_by
string message
```

`prog_cfg` 自身 schema 当前为 2，在 name 之前编码 schema 版本，并在 PTY 后编码 `log_mode`、`log_dir`、`log_file_name`、`log_max_size_kb` 和 `log_archive_count`。所有计数均有界检查。新增或重排字段必须升级协议或内层 schema，不能静默改变布局。

## 日志流

`log`、`start --log` 和 `restart --log` 先返回普通响应；成功后返回 `STREAM` 帧。订阅从当前 generation 最近 64 KiB 开始并实时跟随，generation 结束后发送不带 `MORE` 的最终空帧，且不跨自动重启。活跃数据帧始终设置 `MORE`。每个客户端会话最多排队 1 MiB，慢客户端超限时只断开该会话；daemon 始终继续读取子进程管道、更新 tail 并写盘，不会因慢客户端反压业务进程。

日志文件打开、写入或轮转失败不会阻止受管进程运行。未持久化字节计入 `log_dropped_bytes`，daemon 按 1/2/4/8/16/32/60 秒退避重试；恢复后只写入新数据，不回放已经丢失的数据。有效路径及降级状态通过 list v5 和 inspect v2 暴露。

## 应用 SDK

daemon 启动子进程时注入 `PM_TINY_APP_NAME`、`PM_TINY_SOCK_FILE` 和 `PM_TINY_UDS_ABSTRACT_NAMESPACE`。SDK 使用同一 socket 发送 `0x31` ready 和 `0x32` tick，payload 为应用名称字符串。

Linux/Android daemon 在 accept 后使用 `SO_PEERCRED` 验证本地调用方。daemon 自身有效 UID 始终允许，
其他身份必须列入 `pm_tiny_allowed_uids` 或 `pm_tiny_allowed_gids`；该校验同时适用于文件 socket 和
abstract socket。abstract socket 不具备文件权限边界，不能用 socket 路径权限替代 peer credential。

## 错误处理

收到错误 magic、未知版本、非法 flags、超长 payload、截断头或截断 payload 时，连接必须被视为协议错误并关闭。未知业务命令返回错误响应，不应导致 daemon 崩溃。

## 配置持久化

`save` 会先在临时路径完整写出 YAML 和应用环境目录，再备份旧版本并替换两者；任一步失败都会恢复旧配置和旧环境目录。Linux/Android 环境 sidecar 为 `name.yaml`，内容是 `schema: 1` 和字符串序列 `environment`，可无损保存空值、首尾空格、Unicode、换行和多个等号。文件和目录替换边界执行 `fsync` 并记录事务 journal；Windows 使用 write-through 替换。空配置写为合法的 `[]`，被删除应用遗留的环境文件也会在成功事务中清除。

## Windows 差异

Windows 使用 `\\.\pipe\pm_tiny` 的 byte-stream 模式承载同一 v3 帧格式；命名管道的消息边界不作为协议边界，接收端按头部中的 `payload_len` 重组。daemon 主线程通过单个 Asio `io_context` 驱动 overlapped accept、控制会话、根进程退出通知和日志管道，不为客户端或程序创建线程；不完整帧超过 5 秒或帧非法时仅关闭对应连接。控制管道的 SDDL 在每个 `CreateNamedPipeW` 实例上显式应用，非法 SDDL 会阻止 daemon 启动。除上述结构化响应外，普通响应统一为 `i32 status_code + string message`。`stop/delete/restart` 在 generation 对应的完整进程树操作完成后响应；`log` 与 `start --log` 使用按块发送的 `STREAM` 帧，每会话发送队列上限为 1 MiB，慢客户端只会断开自身。

daemon、CLI 和 daemon 注入给应用 SDK 的管道名均可由 `PM_TINY_PIPE_NAME` 覆盖。未设置时，
Windows CLI 会通过 `PM_TINY_HOME\pm_tiny.yaml` 读取 `pm_tiny_pipe_name`，最后才使用
`\\.\pipe\pm_tiny`。该变量可用于并行实例和自动化测试隔离，三端必须使用相同值。完整优先级见
[daemon_configuration.md](daemon_configuration.md)。

Windows CTest 会运行真实 daemon、CLI 和 SDK，覆盖协议表中定义的全部消息类型，并验证半包/粘包解码、非法 magic/version/flags、超长 payload、慢客户端超时、日志分片、依赖顺序和心跳策略。详见 [windows_port_status.md](windows_port_status.md)。

进程状态整数新增值 `9`，文本表示为 `blocked`，表示程序尚未启动且被失败依赖阻塞。现有 list
字段位置和二进制布局不变，旧客户端会将未知状态按其既有 fallback 显示。

## 排障

Linux/Android 客户端收到成功的 `quit` 响应后，会等待 daemon PID 退出。`/proc/<pid>/stat` 中的
zombie/退出态视为已经退出，避免父进程尚未 `waitpid` 时无限等待；等待超过 30 秒或被 Ctrl+C
中断时，客户端返回非零并提示检查 daemon 状态和日志。该等待属于 CLI 行为，不改变协议响应布局。

```bash
ss -xl | rg pm_tiny
strace -xx -e trace=connect,read,write -f ./build/pm ls
```

检查客户端和 daemon 使用的 socket 配置是否一致，并确认旧版客户端没有继续连接 v3 daemon。
