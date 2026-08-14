# `pm start` 3.1 命令设计

3.0 起将“启动已有定义”和“创建运行时定义”拆成两个明确形式；3.1 保持该协议语义：

```bash
pm start <name> [--log]
pm start <name> [options] -- <executable> [args...]
```

名称始终由用户提供。没有 `--` 时只允许启动 daemon 已有定义；名称不存在立即失败。带 `--` 时只允许创建新定义；同名定义已存在立即失败。`--` 后的参数按操作系统已经完成的 argv 分词结果逐项传输，空字符串、包含空格的参数、顺序以及以 `-` 开头的参数都不会再次解析。

定义选项为 `--cwd`、`--kill-timeout`、`--user`、可重复的 `--env`、可重复的 `--depends-on`、`--start-timeout`、`--failure-action`、`--heartbeat-timeout`、`--oom-score-adj`、`--daemon`/`--no-daemon`、`--pty`/`--no-pty`、五项重启策略，以及 `--log-mode split|combined`、`--log-dir`、`--log-file-name`、`--log-max-size-kb`、`--log-archive-count`。旧的下划线选项、`--name` 和自动名称推导已删除。

动态定义继承 CLI 环境，但过滤所有 `PM_TINY_*`。显式 `--env KEY=VALUE` 覆盖同名继承值；显式设置 `PM_TINY_*` 会在 CLI 解析阶段失败。`pm save` 在三平台都将完整继承快照写入 `schema: 1` 的 YAML sidecar；显式覆盖统一保留在 `prog.yaml` 的 `env_vars`，reload 后仍由显式值优先。3.1 不读取旧的无扩展名逐行环境文件，也不接受旧 Windows `inherited_env` 字段。

PTY 默认关闭。Linux/Android 可显式启用；启用 PTY 且未指定日志模式时自动使用 `combined`，显式组合 `pty: true` 与 `log_mode: split` 会失败。Windows 对 `--pty`、非空 `--user`、非零 `--oom-score-adj` 和非 `skip` 的 `--failure-action` 返回错误。

Linux/Android 与 Windows 的日志目录和文件名规则一致：空 `log_dir` 使用 daemon 的应用日志目录，`combined` 默认 `<name>.log`，`split` 默认 `<name>_stdout.log` 与 `<name>_stderr.log`；配置 `api.log` 时 split 派生为 `api_stdout.log` 与 `api_stderr.log`。当前文件无编号，`.1` 是最新历史文件。`log_archive_count` 表示历史文件数量，允许为 0。Windows 旧字段 `log_size_kb`、`log_files`、`log_file`、`log_file_count` 已删除并明确拒绝。

启动响应分为：

- `started`：进程创建 API 已成功，返回 PID，退出码 0。
- `waiting`：依赖尚未就绪，返回等待来源，退出码 0。
- `blocked`：上游依赖已经失败，返回 `blocked_by`，退出码非零。

动态定义启动失败时会撤销新定义和 DAG 节点，不会留下失败条目。成功创建后会提示执行 `pm save` 持久化。

CLI 退出码统一为：成功启动或正常等待依赖返回 `0`，daemon/业务失败返回 `1`，命令行参数错误返回
`2`，日志流期间由 Ctrl+C 中断返回 `130`。

Linux/Android 通过目标进程最终环境中的 `PATH` 解析 executable，再以结构化 argv 调用
`execve`；Windows 使用相同的目标环境选择规则解析 executable，并将 executable 与按 Windows 标准规则
转义后的命令行分别传给 `CreateProcessW`。

`start --log` 在 `started` 后立即订阅日志；若启动结果为 `waiting`，客户端连接会保留到依赖满足且目标
真正启动，再开始发送日志。Windows 控制面由 daemon 主线程上的单个 `io_context` 驱动控制会话、
根进程退出通知和 overlapped 日志管道，不创建控制连接线程或每程序日志线程。

程序名必须满足 `[A-Za-z0-9][A-Za-z0-9._-]*`，长度为 1 到 128。该规则同时约束配置、动态定义和依赖名称，避免名称进入日志及环境 sidecar 路径时发生路径穿越。
