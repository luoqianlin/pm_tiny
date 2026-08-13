# PM_Tiny 本地 IPC 协议（v2）

## 适用范围

Linux 版 `pm`、Android 版 `pm2`、`pm_tiny` 和 `pm_sdk` 使用 Unix Domain `SOCK_STREAM` 通信。协议 v2 是固定长度头部的二进制协议，不使用 JSON、HTTP 或换行分帧。Android 客户端只改变安装文件名以避开系统 `/system/bin/pm`，协议和 CMake target 仍与 Linux 客户端相同。

Linux daemon 使用 standalone Asio 调度 UDS、客户端 session 和子进程输出管道；CLI 日志等待和 Linux/Windows SDK 也复用 Asio 传输。协议编解码仍由 `protocol_v2` 独立完成，因此传输层不依赖一次读取对应一帧。

socket 地址由 `PM_TINY_SOCK_FILE` 或 `pm_tiny.yaml` 配置；`PM_TINY_UDS_ABSTRACT_NAMESPACE=1` 时使用 Linux abstract namespace。

## 帧格式

每帧由 16 字节头部和 payload 组成：

| 偏移 | 长度 | 字段 | 编码 |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PMT2` |
| 4 | 1 | version | `2` |
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

### `0x23` 进程列表

`0x23` 成功响应在通用的 `status_code` 和 `message` 后附带以下结构：

```text
i32 list_schema_version  // 当前为 2
i32 process_count
repeat process_count times:
  i64 pid                // 不可用时为 -1
  string name
  string cwd
  string command
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
```

列表 schema 版本独立于帧协议版本。当前升级直接替换了旧的 `0x23` 字段布局和 Windows 文本响应，不提供兼容回退；部署时必须同步替换 daemon 与 CLI，新旧版本不能混用。

`pm graph`/`pm dag` 不增加新的消息类型，而是请求同一 `0x23` 列表快照并在客户端构建只读依赖图。
因此其文本、JSON schema v1 和 DOT 输出属于 CLI 接口，不属于 daemon 二进制协议。

命令参数中的名称、路径、命令和环境变量均按字符串编码。`start` 字段顺序以 CLI 构造代码和 daemon 解析代码为准，新增字段必须增加协议版本或尾部可选字段，不能改变既有字段顺序。

## 日志流

`log`、`start --log` 和 `restart --log` 先返回普通响应；成功后返回 `STREAM` 帧。每个流帧的 payload 是 UTF-8 日志片段，设置 `MORE` 表示还有后续数据，最后一帧不设置 `MORE`。

## 应用 SDK

daemon 启动子进程时注入 `PM_TINY_APP_NAME`、`PM_TINY_SOCK_FILE` 和 `PM_TINY_UDS_ABSTRACT_NAMESPACE`。SDK 使用同一 socket 发送 `0x31` ready 和 `0x32` tick，payload 为应用名称字符串。

## 错误处理

收到错误 magic、未知版本、非法 flags、超长 payload、截断头或截断 payload 时，连接必须被视为协议错误并关闭。未知业务命令返回错误响应，不应导致 daemon 崩溃。

## 配置持久化

`save` 会先在临时路径完整写出 YAML 和应用环境目录，再备份旧版本并替换两者；任一步失败都会恢复旧配置和旧环境目录。空配置写为合法的 `[]`，被删除应用遗留的环境文件也会在成功事务中清除。

## Windows 差异

Windows 使用 `\\.\pipe\pm_tiny` 的 byte-stream 模式承载同一 v2 帧格式；命名管道的消息边界不作为协议边界，接收端按头部中的 `payload_len` 重组。服务端使用 Asio overlapped I/O，并为单次读写设置 5 秒超时，超时或非法帧会取消并关闭连接。除上述结构化 `0x23` 响应外，普通响应统一为 `i32 status_code + string message`。当前 Windows 已支持基础进程控制、`save/delete/reload/inspect/log` 和 `ready/tick` 消息；`log` 使用与 Linux 相同的 `STREAM/MORE` 分片标志。

daemon、CLI 和 daemon 注入给应用 SDK 的管道名均可由 `PM_TINY_PIPE_NAME` 覆盖；未设置时仍使用 `\\.\pipe\pm_tiny`。该变量可用于并行实例和自动化测试隔离，三端必须使用相同值。

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

检查客户端和 daemon 使用的 socket 配置是否一致，并确认旧 v1 客户端没有继续连接 v2 daemon。
