# 进程树终止机制

pm_tiny 在 Linux/Android 上为每次启动建立独立的进程树句柄。默认配置
`pm_tiny_process_tree_mode: auto` 优先使用 cgroup v2；内核、挂载或权限不满足时自动降级为进程组，并在日志中记录 `degraded`。

```yaml
pm_tiny_process_tree_mode: auto       # auto | cgroup | process_group
# pm_tiny_cgroup_root: /sys/fs/cgroup
```

cgroup 模式下，停止流程是 `SIGTERM → kill_timeout_s → SIGKILL`。Android 4.19
没有 `cgroup.kill` 时，pm_tiny 会递归读取 `cgroup.procs` 并循环强杀。只有根进程已经
被 `waitpid` 回收且 cgroup 为空，stop、delete、restart、reload 和 quit 才会继续。
如果读取 `cgroup.procs` 或子 cgroup 失败，pm_tiny 会保守地将进程树视为非空，避免
在权限或 I/O 错误时提前完成终止任务。cgroup 探测、attach、递归枚举、信号和清理由
统一的文件系统后端执行，自动降级与强制 cgroup 模式的行为保持一致。

pm_tiny 同时启用 child-subreaper，负责回收根进程退出后被收养的后代。进程组降级模式
使用负 PGID 发信号，普通 fork 后代可以被覆盖；后代若主动执行 `setsid()` 或迁移到其他
cgroup，无法提供 cgroup 模式的强保证。运行在 systemd 下应使用 `KillMode=control-group`。

同一进程树句柄也用于 SDK 身份确认：cgroup 模式从当前应用 cgroup 的 `cgroup.procs` 确认 peer PID，
进程组模式比较 peer 的 PGID。ready/tick 因此只能推进当前 generation，不能由同 UID 的无关进程冒充。

pm_tiny 被不可捕获地 `SIGKILL` 时无法在进程内执行清理；systemd/Android init 负责监督
器级清理，pm_tiny 下次启动也会扫描并清理本实例遗留的 cgroup。

正常启动不会按命令行扫描或终止系统中的外部进程。实例所有权只来自本次启动建立的 cgroup/进程组、
child-subreaper 和上层 systemd/init 监督边界。

Windows 为每次启动创建独立进程组和 Job Object，并在子进程恢复执行前将其加入 Job。
stop、delete、restart、reload、timeout 和 quit 使用统一流程：先向该 generation 的进程组
发送 `CTRL_BREAK_EVENT`，等待 `kill_timeout_s`，再通过 `TerminateJobObject()` 强杀。
只有 Job Object 的活动进程数归零，终止任务才算完成；根进程先退出不会导致后代脱管。
Windows 服务或无共享控制台环境无法发送 CTRL_BREAK 时会立即降级为 Job Object 强杀并记录
警告。所有完成事件、重启定时器和控制操作都会校验 generation，旧实例事件不能修改新实例。

Android 验证时使用 `pm2`（系统的 `pm` 命令属于 Android 包管理器），可通过
`PM_TINY_PROCESS_TREE_MODE` 和 `PM_TINY_CGROUP_ROOT` 覆盖配置。SELinux enforcing
环境需要允许 pm_tinyd 创建子 cgroup、写入 `cgroup.procs` 和删除空 cgroup。

## Android 实机回归

进程树夹具会创建根进程、子进程和孙进程，并让后代持续运行，覆盖 `stop`、`start`、
`restart`、`inspect`、`delete`、`reload`、`quit` 和超时强杀。测试通过 PID 文件确认全部后代
均已退出，并检查 generation、cgroup 模式和日志中的终止结果。夹具源码为
[`scripts/android_process_tree_fixture.sh`](../scripts/android_process_tree_fixture.sh)，仅部署到
`/data/local/tmp`，不会修改设备上的生产配置。

完整回归可在完成 Android Release 构建后执行：

```bash
./scripts/test_android_process_tree.sh <adb-serial>
```

脚本要求显式提供 ADB serial，并使用独立的 `PM_TINY_HOME`、socket、配置和 cgroup 根运行
当前构建产物，不连接生产 daemon。它自动覆盖 cgroup v2、强制 process-group、忽略
`SIGTERM` 后的超时强杀、根进程先退出、restart/delete/reload/quit，以及 daemon 被
`SIGKILL` 后由下一实例清理遗留 cgroup。运行前后都会确认生产 `pm_tiny` PID 仍存活，
并比较生产进程的名称集合及 `online` 状态。日志、PID 和配置保存在
`build/test-artifacts/android/<时间>-<serial>`，远端临时目录在退出时自动删除。

重启策略回归使用独立测试实例，验证自动重启次数限制、抑制状态、`list --json` schema v3、
`inspect` 运行时字段和手动恢复。测试不会替换或停止设备上的生产实例。

可复现命令：

```bash
./scripts/test_android_restart_policy.sh <adb-serial>
```

脚本默认使用 `.build_android/_install/Release`，并将设备日志、两轮 JSON/inspect 输出及
生产状态保存到 `build/test-artifacts/android/<时间>-<serial>-restart`。
