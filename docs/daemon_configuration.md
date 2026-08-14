# Daemon 参数、环境变量与目录

Linux/Android 与 Windows 使用同一套 daemon 公共配置模型。平台进程创建、IPC 和服务管理仍保持
独立实现，但公共字段、覆盖优先级、路径派生和错误处理一致。

## 命令行

公共参数：

```text
-c, --config PATH
--home PATH
--log-level debug|info|warn|error|fatal
--log-max-size-kb N
--log-archive-count N
-h, --help
--version
```

Linux/Android 另支持 `-d/--daemon`。Windows 另支持 `--service`、`--service-name`、
`--pipe-name` 和 `--pipe-sddl`。另一平台的专属参数会作为参数错误退出，不会静默忽略。

各字段优先级为：

```text
CLI > 环境变量 > pm_tiny.yaml > 内置默认值
```

`--config` 只选择 daemon 配置文件；`--home` 只覆盖 home，并为仍为空的日志、程序配置和应用目录
提供派生根目录。显式配置的路径不会因为 `--home` 再次改写。

显式 `--config` 指向不存在或不可读文件时启动失败。未显式指定时，Linux/Android 继续检查
`/usr/local/pm_tiny/pm_tiny.yaml`；Windows 检查 `<effective-home>\pm_tiny.yaml`。隐式配置文件不存在时
使用环境变量和默认值。`prog.yaml` 不存在、零字节、仅含空白或 YAML 注释时，daemon 以零程序状态
启动；不会在启动阶段自动创建该文件，后续可通过 `pm start` 动态添加程序并由 `pm save` 持久化。

## 公共环境变量

| 变量 | 用途 | 未设置时 |
| --- | --- | --- |
| `PM_TINY_HOME` | daemon 数据根目录 | Linux/Android 为当前用户 `~/.pm_tiny`；Windows 前台为 `%USERPROFILE%\.pm_tiny` |
| `PM_TINY_LOG_FILE` | daemon 日志 | `<home>/pm_tiny.log` |
| `PM_TINY_PROG_CFG_FILE` | 程序配置 | `<home>/prog.yaml` |
| `PM_TINY_APP_LOG_DIR` | 应用日志目录 | `<home>/logs` |
| `PM_TINY_APP_ENVIRON_DIR` | 环境快照目录 | `<home>/environ` |
| `PM_TINY_LOG_LEVEL` | daemon 日志级别 | `info` |
| `PM_TINY_LOG_MAX_SIZE_KB` | daemon 日志轮转大小 | `4096` |
| `PM_TINY_LOG_ARCHIVE_COUNT` | daemon 历史日志数量 | `3` |

Linux/Android 平台变量为 `PM_TINY_SOCK_FILE`、`PM_TINY_UDS_ABSTRACT_NAMESPACE`、
`PM_TINY_PROCESS_TREE_MODE` 和 `PM_TINY_CGROUP_ROOT`。Windows 平台变量为
`PM_TINY_PIPE_NAME` 与 `PM_TINY_PIPE_SDDL`。

daemon 会把最终公共值导出到自身环境。Windows 与 Linux/Android 都会向被管理进程注入
`PM_TINY_APP_NAME`、`PM_TINY_HOME` 和本平台 IPC 地址。程序配置和动态 `--env` 禁止覆盖
`PM_TINY_*` 保留变量。

三平台 daemon 使用同一个日志实现和相同文本格式：

```text
时间.微秒 [level] [daemon] message
```

配置解析完成前日志安全输出到控制台；配置完成后按上述级别和轮转参数写入文件。前台运行同时镜像
到控制台，Linux/Android `--daemon` 与 Windows `--service` 默认只写文件。日志文件无法打开或运行中
写入失败时会退回控制台并继续运行，避免日志介质故障直接阻止进程监督。

## YAML 与路径

公共 daemon 字段为：

```yaml
pm_tiny_home_dir: .
pm_tiny_log_file: pm_tiny.log
pm_tiny_prog_cfg_file: prog.yaml
pm_tiny_app_log_dir: logs
pm_tiny_app_environ_dir: environ
pm_tiny_log_level: info
pm_tiny_log_max_size_kb: 4096
pm_tiny_log_archive_count: 3
```

Windows 可增加：

```yaml
pm_tiny_pipe_name: \\.\pipe\pm_tiny
pm_tiny_pipe_sddl: D:P(A;;GA;;;SY)(A;;GA;;;BA)
```

配置文件中的相对路径以 `pm_tiny.yaml` 所在目录为基准。环境变量和 CLI 中的相对路径以启动
daemon 或 CLI 时的工作目录为基准。Windows 参数、环境变量和配置路径使用 UTF-8 内部表示，并通过
宽字符 Win32 API 访问文件，支持非 ASCII 路径。

未知 `pm_tiny_*` 字段、非法类型和超出范围的值会导致明确错误。socket/cgroup 与 pipe/SDDL 等
平台专属字段不能放到另一平台配置中。

程序配置必须是 YAML 顶层数组。合法的 `[]` 以及不存在或没有实际内容的文件表示空配置；显式
`null`/`~`、scalar、mapping、损坏 YAML 和无效程序条目仍会导致加载失败。启动和 `pm reload` 使用
相同语义，因此删除或清空 `prog.yaml` 后 reload 会停止并移除当前全部程序定义。

## Windows SCM

服务安装脚本的 `-HomePath` 默认是 `%ProgramData%\pm_tiny`：

```powershell
.\scripts\windows\install-service.ps1 `
  -BinaryPath .\pm_tiny.exe `
  -ConfigPath .\pm_tiny.yaml `
  -HomePath "$env:ProgramData\pm_tiny" `
  -Start
```

脚本把 `PM_TINY_HOME`、`PM_TINY_PIPE_NAME`、`PM_TINY_PIPE_SDDL` 和受控 `PATH` 写入服务注册表
环境。`pm.exe` 查找控制管道的顺序为 `PM_TINY_PIPE_NAME`、
`PM_TINY_HOME\pm_tiny.yaml` 中的 `pm_tiny_pipe_name`、默认 `\\.\pipe\pm_tiny`。连接使用自定义
服务 pipe 时，交互用户需要设置 pipe，或把 `PM_TINY_HOME` 指向对应服务 home。

## 环境快照迁移

`pm save` 在三平台统一写入 `<PM_TINY_APP_ENVIRON_DIR>/<name>.yaml`：

```yaml
schema: 1
environment:
  - KEY=VALUE
```

`prog.yaml` 只保存显式 `env_vars`。旧 Windows `inherited_env` 字段和旧无扩展名环境文件均被明确
拒绝，不提供双格式兼容。迁移时删除 `inherited_env`，把其中字符串序列写入对应的 YAML sidecar。

Windows 使用 journal、临时配置、环境 staging 目录和 `MoveFileExW` write-through 替换同时提交
`prog.yaml` 与 `environ`；daemon 启动和 reload 前会恢复中断事务。

## 查询实际生效值

运行中的 daemon 可通过以下只读命令查询：

```bash
pm info
pm info --json
# Android
pm2 info --json
```

输出展示最终值及其来源：`default`、`config_file`、`environment`、`command_line` 或 `derived`。
同时包含前台/后台/SCM 模式、PID、uptime、运行状态、持久化任务、文件与运行时定义数量、实际日志
sink、进程树后端和平台能力。日志文件打开或写入失败后，sink 显示为 `console_fallback`，并保留最后
错误。命令不会输出 daemon 的完整继承环境或受管程序 `env_vars`；进程级详情继续使用 `pm inspect`。
