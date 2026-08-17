# Windows Port Status

## 当前能力
- 控制管道已启用，可通过 pm list|start|stop|quit 与守护进程交互。
- `pm list` 与 Linux/Android 共用结构化列表协议、runtime snapshot 和渲染器，支持自适应表格、`--wide`、`--json` 与 `--no-color`；Windows 的 PTY 字段显示为不支持。
- `pm inspect` 与 Linux/Android 使用同一二进制 schema 和 CLI renderer，不再返回 Windows 特有的文本键值响应；平台差异通过 `pty`、`process_tree_backend` 和 capability 字段表达。
- `pm info [--json]` 与 Linux/Android 使用同一 daemon-info schema，报告前台或 SCM `service` 模式、named pipe/SDDL、Job Object、日志 sink、最终配置来源和 Windows 能力差异。
- `pm graph` 与 Linux/Android 共用客户端依赖图渲染器，支持全图/聚焦子图、文本、JSON、DOT 和状态颜色，不新增命名管道协议命令。
- 支持在运行时动态 start/stop 配置中定义的进程，停止后守护进程保持待命。
- 默认读取 `pm_tiny.yaml`，并通过 `pm_tiny_prog_cfg_file` 指向 `prog.yaml`；daemon 配置文件名、
  程序配置分层及日志字段与 Linux/Android 一致。
- 使用 CreateProcessW 启动子进程，支持守护模式、指数退避、时间窗口重启限流、稳定运行复位及手动恢复。
- 每个被监管程序使用独立进程组，并在挂起状态下加入独立 Job Object。stop、delete、restart、timeout、reload 和 quit 先发送 CTRL_BREAK，等待 `kill_timeout_s` 后再强杀完整进程树。
- Linux/Android 与 Windows 共用 `src/core/termination_job.*` 终止状态机，按 generation 隔离实例；根进程退出由同一 `io_context` 上的 `object_handle` 异步通知，退出后仍等待 Job Object 清空，不会遗漏后代。
- 分别捕获 stdout/stderr，按公共 `split`/`combined` 规则写入日志；默认目录为配置文件同级 `logs`，文件名、大小、`.1` 历史命名、保留数和降级重试语义与 Linux 一致。每个 generation 使用 overlapped 命名管道接入同一 `io_context`，不为程序创建日志线程。
- 响应控制台 Ctrl+C/Close 事件，优雅停止子进程并清理资源。
- Windows SDK 4.0 已生成 `pm_sdk`，使用可取消 overlapped named pipe 持久连接发送 v3
  `APP_READY`/`APP_TICK`，与 Linux/Android 共用事件合并、状态快照和退避语义。
- standalone Asio 1.30.2 已固定在 `dependencies/asio`，CMake 配置不需要访问 GitHub，并会拒绝缺失或版本错误的副本。
- 控制管道 accept、控制会话、根进程退出和日志读取共用 daemon 主线程上的一个 `io_context`，不为连接或程序创建线程。不完整帧超过 5 秒后关闭，其余连接和进程状态机继续运行；正常等待日志的空闲会话不受该超时影响。运行时按最近的启动、心跳、重启和终止期限使用 `run_one_for` 等待，任一异步事件到达即返回状态机，空闲等待上限为 1 秒，仅在持久化任务和 Job Object drain 阶段保留短周期检查。
- 默认控制管道 SDDL 允许 `SYSTEM`、本机管理员和本机交互登录用户，普通用户可直接以前台方式运行 daemon 和 CLI；控制管道拒绝远程客户端。`--pipe-sddl` 会应用到每一个 `CreateNamedPipeW` 实例，非法 SDDL 会导致 daemon 明确启动失败。
- `stop/delete/restart` 仅在对应 generation 的完整 Job Object 进程树完成终止（以及 restart 新 generation 启动）后响应成功；旧 generation 事件不能完成新请求。
- `log` 和 `start --log` 发送当前 generation 最近 64 KiB 后实时跟随；`log --history` 只对已停止且有
  内存缓存的最后一代输出带 generation/UTC 退出时间/PID/退出状态的提示和缓存，不伪造实时退出事件。
  普通 `log` 在自动重启退避期间等待下一 generation，启动后只输出新实例日志，不回放旧实例缓存。
  `restart --log` 等旧 Job Object 清空及新 generation 启动成功后才绑定新实例，不输出旧实例缓存或退出事件；
  每会话队列限制为 1 MiB。
- Windows 控制台中的 `pm`、`pm_tiny` 结构化文本和 daemon 前台日志使用 UTF-8/UTF-16 控制台写出；重定向
  输出和 daemon 日志文件保持无 BOM UTF-8。`pm log` 中的受管程序 stdout/stderr 保留程序原始字节，不能
  假定其编码；supervisor 退出事件单独按 UTF-8 文本输出。
- daemon、CLI 和 SDK 支持通过 `PM_TINY_PIPE_NAME` 选择同一命名管道；`pm.exe` 未设置该变量时会从
  `PM_TINY_HOME\pm_tiny.yaml` 读取 `pm_tiny_pipe_name`，最后使用 `\\.\pipe\pm_tiny`。
- Windows 已实现与 Linux/Android 相同的 `PM_TINY_HOME`、日志、程序配置、应用日志和环境快照
  路径派生；前台默认 `%USERPROFILE%\.pm_tiny`，SCM 安装默认 `%ProgramData%\pm_tiny`。
- `depends_on` 使用跨平台公共不可变依赖图和启动状态机，统一校验、ready 解锁、failed/blocked 传播与恢复；退出和 reload 按逆拓扑顺序停止。
- Windows CTest 已覆盖全部 v3 命令、日志分片、SDK ready/tick、依赖顺序、非法 reload 原子性、并发控制、启动/心跳超时、异常帧恢复、100 进程线程数稳定、1000 次短连接句柄及 Private Bytes 稳定、非法 SDDL 拒绝、慢日志客户端隔离及 `pm log` Ctrl+C 返回 130。

## 与 Linux 的功能差异

当前 Windows 与 Linux 已统一以下公共接口：

- CLI 命令均为 `list/graph/start/stop/restart/delete/log/inspect/info/save/reload/quit`，动态
  `start`、结构化 argv、环境继承和 `--log` 语义相同。
- 使用同一协议 v3、start request/response、list schema v5、inspect schema v2、依赖状态机、
  重启策略和 CLI renderer。
- `waiting/starting/online/failed/blocked`、generation、ready/tick、最后退出、重启状态和
  `config_source` 等公共字段含义相同。
- `stop/delete/restart/reload/timeout/quit` 均等待对应 generation 的完整进程树终止，不以根进程退出
  作为完成条件。

仍然存在的差异如下。除“Windows 尚未支持”项外，其余属于操作系统机制不同，不应通过伪造相同实现来隐藏。

| 能力 | Linux | Windows | 当前约定 |
| --- | --- | --- | --- |
| 本地 IPC | Unix Domain Socket | byte-stream named pipe | 协议和响应字段相同，地址配置分别使用 `PM_TINY_SOCK_FILE` 和 `PM_TINY_PIPE_NAME` |
| IPC 调用方权限 | `SO_PEERCRED`，支持 UID/GID allowlist | SDDL ACL，默认允许 `SYSTEM`、管理员和交互用户 | 均在接受控制连接时校验，不开放默认远程控制面 |
| 进程创建 | `execvpe` 使用结构化 argv | `CreateProcessW`，按 Windows 标准规则生成命令行 | executable 与 args 在配置和协议中保持分离 |
| 进程树 | cgroup v2，失败时可观测地降级为进程组，并配合 child-subreaper | 每程序独立 Job Object 和进程组 | `process_tree_backend` 返回真实后端，不统一为虚构名称 |
| 优雅终止 | POSIX signal，超时后强制终止进程树 | 优先发送 `CTRL_BREAK`，超时或无共享控制台时使用 Job Object 强杀 | 降级必须记录日志；两端均等待完整进程树清空 |
| PTY | 支持 `pty: true` | 尚不支持，返回 `unsupported` | Windows 配置和动态 start 明确拒绝 `pty: true` |
| 指定运行用户 | 支持非空 `user` | 尚不支持 | Windows 明确拒绝，不静默使用 daemon 身份启动 |
| OOM 调整 | 支持 `oom_score_adj` | 仅接受默认值 0 | 这是 Linux/Android 内核能力；后续资源限制应分别映射到 cgroup 和 Job Object |
| failure action | 支持 `skip/restart/reboot` | 当前仅支持 `skip` | Windows 明确拒绝其他值；跨平台 `reboot` 需先定义权限和失败语义 |
| 最后退出原因 | 区分正常退出与 signal，并返回退出码或信号编号 | 返回正常退出及 Windows exit code | 字段相同，但不在 Windows 伪造 POSIX signal |
| 系统服务集成 | Ubuntu systemd 安装脚本 | Windows SCM 安装、状态和卸载脚本 | 服务管理保持平台原生实现 |
| 发布与升级 | 版本目录、原子 `current` 链接和 journal 回滚 | 版本目录、`current.release`、SCM BinaryPath journal 回滚 | manifest/SHA-256、隔离健康检查和失败恢复语义一致 |

后续缩小差异时，优先考虑 Windows 指定用户运行、ConPTY 可行性和
`failure_action` 跨平台语义。`oom_score_adj` 不应在 Windows 创建无实际作用的
兼容字段；更合理的方向是统一资源限制配置，并分别映射到 Linux cgroup 和 Windows Job Object。

## 当前限制

- `user`、非零 `oom_score_adj`、`pty: true` 和非 `skip` 的 `failure_action` 尚不支持，配置加载时会明确拒绝。
- Windows 服务或 SSH 会话通常没有与被管理进程共享的控制台，CTRL_BREAK 不可用时会降级为 Job Object 强杀。
- 已提供 MSVC 二进制版本目录、manifest 校验、隔离预检、SCM BinaryPath 切换及自动回滚脚本；标准安装包格式仍未确定。
- Linux/Android 与 Windows 已共享 CLI 命令定义、公共配置字段及 YAML 序列化、依赖校验、控制命令、重启策略、runtime snapshot、inspect/list 二进制 schema、终止状态机和部分控制操作完成判定；平台事件循环、进程创建、IPC 和终止系统调用仍分别实现，需要继续收敛契约而不强行抽象平台后端。

跨平台后续方向不在本文重复维护，统一见 [`project_roadmap.md`](project_roadmap.md)。

## Windows 服务化

服务入口使用 `pm_tiny.exe --service`，由 SCM 负责启动、停止和关机通知。安装、卸载和
状态查询脚本分别为：

```powershell
.\scripts\windows\install-service.ps1 -BinaryPath .\pm_tiny.exe -ConfigPath .\pm_tiny.yaml `
  -HomePath "$env:ProgramData\pm_tiny" -Start
.\scripts\windows\status-service.ps1
.\scripts\windows\uninstall-service.ps1
```

默认命名管道 SDDL 允许 `SYSTEM` 和本机管理员完全控制，并允许本机交互登录用户读写；安装时可通过
`-PipeSddl` 收紧权限或指定用户 SID。服务实例支持 `-PipeName`，避免测试和多实例之间复用管道。安装脚本会把
`PM_TINY_HOME`、pipe、SDDL 和服务 `PATH` 写入服务注册表环境。本阶段以 VS2022/MSVC x64 为
Windows 发布工具链。

Windows daemon 日志配置使用与 Linux 相同的字段和默认值：

```yaml
pm_tiny_log_file: pm_tiny.log
pm_tiny_home_dir: .
pm_tiny_app_log_dir: logs
pm_tiny_app_environ_dir: environ
pm_tiny_prog_cfg_file: prog.yaml
pm_tiny_pipe_name: \\.\pipe\pm_tiny
pm_tiny_pipe_sddl: D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)
pm_tiny_log_level: info
pm_tiny_log_max_size_kb: 4096
pm_tiny_log_archive_count: 3
```

相对路径按 `pm_tiny.yaml` 所在目录解析。优先级统一为 CLI、环境变量、daemon 配置、内置默认值；
公共路径字段还可分别由 `PM_TINY_LOG_FILE`、`PM_TINY_APP_LOG_DIR`、
`PM_TINY_APP_ENVIRON_DIR` 和 `PM_TINY_PROG_CFG_FILE` 覆盖。Windows 参数和环境变量通过宽字符
Win32 API 读取，支持 Unicode 路径。完整规则见 [daemon_configuration.md](daemon_configuration.md)。

`prog.yaml` 不存在、零字节、仅含空白或注释时，Windows daemon 与 Linux/Android 一样以零程序
状态启动；启动本身不创建该文件，后续可通过 `pm start` 添加程序并由 `pm save` 持久化。reload
读取到上述空配置时会停止并移除全部程序定义。

`pm save` 与 Linux/Android 一样把继承环境写入 `environ\<name>.yaml`，`prog.yaml` 只保存显式
`env_vars`。旧 Windows `inherited_env` 会明确报错。Windows 使用 journal、staging 目录、备份和
`MoveFileExW` write-through 同时提交程序配置与环境目录，启动和 reload 会恢复中断事务。

SCM 集成测试会实际安装临时服务、验证 `version` 管道响应，并停止服务后确认父子进程
均已退出；测试脚本为 `tests/integration/windows/windows_service_integration.ps1`。
发布事务测试还会使用唯一临时服务注入切换前/后失败及中断，验证 stable pointer、SCM BinaryPath、
journal 恢复和外置状态保持不变；脚本为
`tests/integration/windows/windows_release_transaction_integration.ps1`。使用方式和产物见
[`testing_platform.md`](testing_platform.md)。

## 自动化验证

### MSVC 构建与测试

Windows 验证使用 VS 2022/MSVC x64 Release 构建。在 VS 2022 x64 Developer Command Prompt 中运行：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DPM_TINY_BUILD_TESTS=ON
cmake --build build-msvc --config Release --parallel 4
ctest --test-dir build-msvc -C Release --output-on-failure
```

完整 CTest 必须包含 `windows_protocol_integration`、`windows_lifecycle_transition_integration`、
`windows_dependency_mutation_integration`、`windows_concurrent_control_integration` 和 MSVC 专属的
`windows_service_integration`、`windows_release_transaction_integration`。
生命周期公共代码变更还应运行 `termination_job_test` 和 `control_operation_test`。构建后检查：

```powershell
Test-Path .\build-msvc\Release\pm_tiny.exe
Test-Path .\build-msvc\Release\pm.exe
Test-Path .\build-msvc\Release\pm2.exe
```

前两项应为 `True`，`pm2.exe` 应为 `False`。

### 调试注意事项

- 调试 daemon 前使用唯一的 `PM_TINY_PIPE_NAME`、独立配置和日志目录，避免连接正在运行的正式实例。
- named pipe、进程退出通知和日志管道共用 daemon 单线程 `io_context`；断点暂停主线程时，控制命令、日志和退出事件都会一起停滞，不能据此判断为死锁。
- 通过 SSH 或 SCM 启动时通常不能投递 `CTRL_BREAK`，出现降级到 Job Object 强杀的日志属于预期平台行为。
- 测试失败后检查 `build-msvc/test-artifacts/windows*` 下的核心、服务、生命周期、依赖和并发场景制品，再检查是否残留同名 pipe、临时服务或测试进程。
- 调试 `stop/delete/restart` 时同时记录进程名、PID 和 generation；旧 generation 的退出事件不能完成新 generation 的控制请求。
- 修改 Windows 进程创建、终止、协议或公共生命周期代码后，不能只运行单元测试，必须运行完整 Release CTest。

Windows 构建后运行：

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure
```

`windows_protocol_integration` 在独立临时目录和唯一命名管道中启动 daemon，覆盖 `list/graph/stop/start/save/delete/restart/version/log/ready/tick/inspect/info/reload/quit`。测试还会验证依赖图文本/JSON/DOT、日志流、依赖拓扑顺序、空配置保存和 reload、start/heartbeat timeout、异常帧恢复、崩溃循环抑制及手动恢复、100 个进程且 daemon 线程数不线性增长、1000 次真实请求后句柄和 Private Bytes 不线性增长、慢日志客户端独立断开、`pm log` Ctrl+C 返回 130，以及 CTRL_BREAK 优雅退出、超时强杀、根进程先退出、完整后代清理和快速连续重启。生命周期迁移、依赖变更和并发控制使用独立 CTest 项及产物目录，便于执行 `ctest --repeat until-fail:10 -R "(lifecycle_transition|dependency_mutation|concurrent_control)"`。失败现场保留在 `build-msvc/test-artifacts/windows*`。

Windows 服务、SSH 会话或无共享控制台启动方式可能无法投递 CTRL_BREAK；此时会记录 degraded 警告并立即调用 `TerminateJobObject()`。daemon 自身使用 Ctrl+C、控制台关闭或系统关机请求退出，CTRL_BREAK 保留给被管理进程组。

## 基础闭环验证

可使用 CMake 和受支持的 Windows 工具链构建，产物为 `pm_tiny.exe` 和 `pm.exe`。正式 Windows
验证以 Visual Studio 2022/MSVC 的 Release 构建和 CTest 为准。

已实际验证命名管道文本协议：

- `list`：列出运行中的 `demo_ping`。
- `stop demo_ping`：停止守护进程并清空列表。
- `start demo_env`：重新启动配置中的程序。
- `help`：返回命令帮助。
- 未知命令：返回 `ERR unknown command`。
- 不存在的进程：返回 `ERR process not found`。
- `quit`：控制 daemon 退出。
- 子进程环境变量 `TEST_FROM_PM=from_config` 已写入日志并验证。

自动化测试覆盖 Windows 命名管道上的 v3 二进制闭环，包括 `list`、`version`、`stop`、
`start`、`inspect`、`log`（含 `STREAM/MORE`）、`save`、`reload`、`delete`、`quit`，以及 SDK
ready/tick 探针。
