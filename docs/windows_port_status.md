# Windows Port Status

## 当前能力
- 控制管道已启用，可通过 pm list|start|stop|quit 与守护进程交互。
- `pm list` 与 Linux/Android 共用结构化列表协议和渲染器，支持自适应表格、`--wide`、`--json` 与 `--no-color`；Windows 的 PTY 字段显示为不支持。
- `pm graph` 与 Linux/Android 共用客户端依赖图渲染器，支持全图/聚焦子图、文本、JSON、DOT 和状态颜色，不新增命名管道协议命令。
- 支持在运行时动态 start/stop 配置中定义的进程，停止后守护进程保持待命。
- 读取 pm_tiny_win.yaml 并解析与 Linux 版本一致的核心字段（依赖、环境变量、日志配置等）。
- 使用 CreateProcessW 启动子进程，支持守护模式、指数退避、时间窗口重启限流、稳定运行复位及手动恢复。
- 每个被监管程序使用独立进程组，并在挂起状态下加入独立 Job Object。stop、delete、restart、timeout、reload 和 quit 先发送 CTRL_BREAK，等待 `kill_timeout_s` 后再强杀完整进程树。
- 终止状态机按 generation 隔离实例，等待和重启延迟不持有全局进程锁；根进程退出后仍等待 Job Object 清空，不会遗漏后代。
- 捕获标准输出/错误并写入按大小轮转的日志文件（默认 logs/<name>.log，可配置目录/大小/保留数）。
- 响应控制台 Ctrl+C/Close 事件，优雅停止子进程并清理资源。
- Windows SDK 已生成 `pm_sdk`，通过 standalone Asio 的 overlapped named pipe 发送 v2 `APP_READY`/`APP_TICK`。
- standalone Asio 1.30.2 已固定在 `dependencies/asio`，CMake 配置不需要访问 GitHub，并会拒绝缺失或版本错误的副本。
- 控制服务使用 Asio overlapped byte-stream named pipe，单次读写超时为 5 秒；慢客户端超时后会取消连接并继续接受新客户端。
- daemon、CLI 和 SDK 支持通过 `PM_TINY_PIPE_NAME` 选择同一命名管道，默认仍为 `\\.\pipe\pm_tiny`。
- `depends_on` 使用跨平台公共不可变依赖图和启动状态机，统一校验、ready 解锁、failed/blocked 传播与恢复；退出和 reload 按逆拓扑顺序停止。
- Windows CTest 已覆盖全部 v2 命令、日志分片、SDK ready/tick、依赖顺序、启动/心跳超时及异常帧恢复。

## 当前限制

- `user`、非零 `oom_score_adj`、`pty: true` 和非 `skip` 的 `failure_action` 尚不支持，配置加载时会明确拒绝。
- Windows 服务或 SSH 会话通常没有与被管理进程共享的控制台，CTRL_BREAK 不可用时会降级为 Job Object 强杀。
- 当前已有服务安装、卸载和状态脚本，但还没有标准化的 MSVC 发布包、原地升级和自动回滚流程。
- Linux/Android 与 Windows 已共享 CLI 命令定义、公共配置字段及 YAML 序列化、依赖校验、控制命令和重启策略；平台事件循环、进程创建/终止与部分 inspect 输出仍分别实现，需要继续收敛契约而不强行抽象平台后端。

跨平台后续方向不在本文重复维护，统一见 [`project_roadmap.md`](project_roadmap.md)。

## Windows 服务化

服务入口使用 `pm_tiny.exe --service`，由 SCM 负责启动、停止和关机通知。安装、卸载和
状态查询脚本分别为：

```powershell
.\scripts\windows\install-service.ps1 -BinaryPath .\pm_tiny.exe -ConfigPath .\pm_tiny_win.yaml -Start
.\scripts\windows\status-service.ps1
.\scripts\windows\uninstall-service.ps1
```

默认命名管道 SDDL 只允许 `SYSTEM` 和本机管理员；安装时可通过 `-PipeSddl` 增加指定
服务用户 SID。服务实例支持 `-PipeName`，避免测试和多实例之间复用管道。本阶段以
VS2022/MSVC x64 为 Windows 发布工具链。

SCM 集成测试会实际安装临时服务、验证 `version` 管道响应，并停止服务后确认父子进程
均已退出；测试脚本为 `test/windows_service_integration.ps1`。

## 自动化验证

Windows 构建后运行：

```powershell
ctest --test-dir build --output-on-failure
```

`windows_protocol_integration` 在独立临时目录和唯一命名管道中启动 daemon，覆盖 `list/graph/stop/start/save/delete/restart/version/log/ready/tick/inspect/reload/quit`。测试还会验证依赖图文本/JSON/DOT、超过 2 MiB 的 `STREAM/MORE` 日志、依赖拓扑顺序、空配置保存和 reload、start/heartbeat timeout、异常帧恢复、崩溃循环抑制及手动恢复，以及 CTRL_BREAK 优雅退出、超时强杀、根进程先退出、完整后代清理和快速连续重启。失败现场保留在 `build/test-artifacts/windows`。

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

自动化测试覆盖 Windows 命名管道上的 v2 二进制闭环，包括 `list`、`version`、`stop`、
`start`、`inspect`、`log`（含 `STREAM/MORE`）、`save`、`reload`、`delete`、`quit`，以及 SDK
ready/tick 探针。
