# PM_Tiny 基准与同类项目对比

## 测量原则

本页将 PM_Tiny 的可重复实测数据与同类项目的架构定位分开记录。不同语言、版本、配置和主机上的资源数字不能直接横向比较，因此不引用无法在同一环境复现的第三方性能数字。

运行 Linux 基线：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DPM_TINY_BUILD_TESTS=OFF
cmake --build build-release -j4
./scripts/benchmark_linux.sh build-release 20
```

脚本已适配 PM_Tiny 3.x 的协议、`pm start <name> [options] -- <executable>` 语法和结构化程序配置。它启动隔离的 daemon 和测试进程，使用独立 `PM_TINY_HOME`、配置、日志和 Unix socket，不连接系统中已有的 PM_Tiny 实例。它测量：

- `pm_tiny` 和 `pm` 二进制大小；
- daemon 空载及管理指定数量进程时的 RSS；
- 30 次 `list --json` 的中位数和 P95 延迟；
- 启动并观察指定数量进程全部 online 的耗时；
- 配置 1000 ms 重启延迟时，两次崩溃启动之间的实际间隔。

默认输出保存在 `build/benchmarks/linux-baseline.json`。结果会受到 CPU 调频、系统负载、内核、编译器和文件系统影响，应在目标设备和正式发布工具链上重新运行。Android 客户端文件名为 `pm2`，与 Node.js PM2 无关；本脚本只针对 Linux。Windows 的正式验证工具链是 VS 2022/MSVC x64，不使用 Linux 数据推断 Windows 性能。

## 3.2.0 Linux 基线

仓库恢复本基准时，使用上述脚本对 3.2.0 Release 构建完成了一次重新测量。原始结果见 [`benchmark-linux-x86_64-3.2.0.json`](benchmarks/benchmark-linux-x86_64-3.2.0.json)。参考环境与下方 2.0.0 历史基线相同，因此适合观察同一主机上的版本变化，但单次结果仍会受当时系统负载影响。

| 指标 | 实测结果 |
| --- | ---: |
| `pm_tiny` 二进制 | 1,641,688 bytes |
| `pm` 二进制 | 1,652,680 bytes |
| daemon 空载 RSS | 5,000 KiB |
| 管理 20 个进程时 daemon RSS | 6,788 KiB |
| `list --json` 延迟中位数（30 次） | 1.898 ms |
| `list --json` 延迟 P95（30 次） | 2.170 ms |
| 20 个进程全部进入 online | 910.636 ms |
| 配置 1000 ms 重启延迟后的实测启动间隔 | 1,007.042 ms |

## 2.0.0 Linux 历史基线

本节由 2.0.0 Release 构建在一台参考开发机上实测，具体环境与精确数值见 [`benchmark-linux-x86_64.json`](benchmarks/benchmark-linux-x86_64.json)。这些数字是历史基线，不代表 4.1.0 的当前实测结果，也不构成所有设备上的性能保证。

参考环境：Ubuntu x86_64、Linux 6.8.0、Intel Core i7-9700、8 个逻辑 CPU、GCC 13.3、Release 构建。

| 指标 | 实测结果 |
| --- | ---: |
| `pm_tiny` 二进制 | 1,351,552 bytes |
| `pm` 二进制 | 1,370,112 bytes |
| daemon 空载 RSS | 4,876 KiB |
| 管理 20 个进程时 daemon RSS | 5,560 KiB |
| `list --json` 延迟中位数（30 次） | 1.854 ms |
| `list --json` 延迟 P95（30 次） | 2.167 ms |
| 20 个进程全部进入 online | 200.161 ms |
| 配置 1000 ms 重启延迟后的实测启动间隔 | 1,106.466 ms |

这些结果包含 CLI 进程创建、IPC 往返和状态序列化开销。二进制未执行 `strip`；Release 压缩包中的文件大小可能不同。

## 架构能力对比

| 项目 | 主要运行时 | 主要平台 | 内置依赖图 | 完整进程树后端 | 默认控制面 | PM_Tiny 的取舍 |
| --- | --- | --- | --- | --- | --- | --- |
| PM_Tiny 3.2 | 原生 C++14 | Linux、Android、Windows | 支持稳定 DAG 和阻塞恢复 | cgroup v2 / process group / Job Object | UDS / named pipe | 面向设备、离线部署和跨平台一致语义 |
| PM2 | Node.js | 以 Node.js 应用环境为主 | 不是核心定位 | 依赖操作系统和 PM2 运行模型 | CLI/daemon，可接入云服务 | PM_Tiny 不提供 Node cluster 和负载均衡 |
| Supervisor | Python | Unix | 主要为进程组和优先级配置 | Unix 进程组语义 | Unix socket / TCP XML-RPC | PM_Tiny 避免 Python 运行时并覆盖 Android/Windows |
| process-compose | Go | 多平台开发与本地编排 | 支持丰富依赖和健康条件 | 侧重应用编排 | CLI/TUI/API | PM_Tiny 保持更小的设备端控制面，不内置 TUI/HTTP 服务 |
| systemd | 原生系统组件 | Linux | 原生 unit 依赖 | cgroup | 本机 D-Bus 等 | PM_Tiny 不替代 init，换取三平台应用层一致性 |
| s6/runit | 原生小型工具 | Unix/Linux | 通过服务目录和脚本组合 | 监督树/平台机制 | 本地工具 | PM_Tiny 提供集中 YAML、结构化状态和依赖图 CLI |
| WinSW | .NET | Windows | 单服务包装，不是编排器 | Windows service/process | SCM | PM_Tiny 同时管理多个依赖进程并共享跨平台协议 |

PM_Tiny 的主要优势不是在所有场景替代这些成熟项目，而是在需要同一套应用监督语义覆盖 Linux、Android 和 Windows，且要求低依赖、离线构建、本地 IPC、依赖恢复和完整进程树终止时提供一个聚焦方案。

## 资料来源

- [PM2](https://github.com/Unitech/pm2)
- [Supervisor](https://github.com/Supervisor/supervisor)
- [process-compose](https://github.com/F1bonacc1/process-compose)
- [systemd](https://systemd.io/)
- [s6-overlay](https://github.com/just-containers/s6-overlay) 与 [runit](https://smarden.org/runit/)
- [WinSW](https://github.com/winsw/winsw)
