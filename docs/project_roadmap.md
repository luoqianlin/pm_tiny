# PM_Tiny 项目定位、竞品分析与发展路线图

> 资料检索日期：2026-08-13。竞品能力与维护状态以其官方仓库和官方文档为准。

## 项目定位

PM_Tiny 的首要定位是面向嵌入式设备和边缘节点的轻量级、离线可部署、跨平台应用进程监督器，而不是通用 init、容器编排系统或云端设备管理平台。

核心目标是：在 Linux、Android 和 Windows 上，以较低资源占用可靠地启动、监控、停止和恢复一组存在依赖关系的本地应用，并提供适合 SSH 和自动化脚本使用的 CLI、结构化状态与诊断信息。

当前已经具备的基础包括：

- 依赖 DAG、异常重启、ready/tick 心跳、启动与心跳超时。
- Linux/Android cgroup v2 进程树管理及进程组降级，Windows Job Object 进程树管理。
- Unix Domain Socket / Windows named pipe 上的二进制协议 v3。
- 动态 start/stop/restart/delete、save/reload、inspect、日志流和结构化进程列表。
- 日志轮转、Linux systemd 集成、Windows SCM 服务化及三平台构建与回归验证手段。

## 同类项目比较

| 项目 | 主要定位 | 值得借鉴 | 不适合直接复制 |
| --- | --- | --- | --- |
| [PM2](https://github.com/Unitech/pm2) | Node.js/Bun 生产进程管理器 | 清晰的 CLI、进程状态与日志体验、启动脚本、持久化工作流 | Node cluster、内置负载均衡、云监控绑定 |
| [Supervisor](https://github.com/Supervisor/supervisor) | Unix 客户端/服务端进程控制系统 | 成熟的状态模型、事件订阅、控制接口和故障状态表达 | Python 运行时依赖、XML-RPC 远程控制面 |
| [systemd](https://systemd.io/) | Linux 系统和服务管理器 | restart 限流、watchdog、资源控制、启动完成通知和明确的失败状态 | 完整 init 职责、Linux 专属的大型能力集合 |
| [s6-overlay](https://github.com/just-containers/s6-overlay) / [runit](https://smarden.org/runit/) | 小型、确定性的服务监督 | 简单监督循环、可预测退出语义、较小运行时表面 | 以目录和脚本替代 PM_Tiny 当前配置及 CLI 模型 |
| [process-compose](https://github.com/F1bonacc1/process-compose) | 非容器应用编排和开发工作流 | readiness/liveness、条件依赖、配置校验、依赖图和 API 分层 | TUI、MCP、cron 调度以及默认常驻 HTTP API |
| [WinSW](https://github.com/winsw/winsw) | Windows 服务包装器 | 服务安装、恢复策略、发布包和升级体验 | 仅包装单个 Windows 服务，不能替代跨平台监督核心 |

GitHub Star、语言和功能数量只用于了解项目生态，不作为 PM_Tiny 技术选型依据。PM_Tiny 更关心资源成本、故障边界、离线部署、跨平台一致性和设备上的可验证恢复能力。

## 发展原则

1. **可靠性优先于功能数量**：先解决崩溃循环、错误配置、进程树泄漏和升级回滚，再增加交互功能。
2. **核心保持轻量**：未启用的健康检查、指标或扩展能力不得增加明显的常驻线程、网络端口和依赖。
3. **本地控制优先**：默认只开放本机 IPC，并验证调用方身份；远程能力放在可选 sidecar 中。
4. **跨平台语义一致**：配置、状态、错误码和生命周期行为尽量一致，平台不支持时显式报告而非静默忽略。
5. **可降级但必须可观察**：cgroup、权限、健康检查和优雅终止降级时，CLI、日志及结构化状态必须说明原因。
6. **变更可验证、可回滚**：配置 reload、协议升级和安装包升级都要有校验、事务边界及回滚路径。

## 阶段一：生产可靠性基线

目标是让 PM_Tiny 可以作为长期运行设备上的稳定基础服务。

- 继续收敛 Linux/Android 与 Windows 的生命周期事件循环；CLI 命令、公共配置模型、依赖图与启动状态机、控制命令和重启策略已共享核心实现。
- 显式重启策略已具备指数退避、最大延迟、时间窗口内最大次数、稳定运行复位、手动恢复及跨平台状态观测；后续补充最后退出原因和更完整的故障分类。
- 增加 `pm validate`：只解析和校验配置、依赖图、路径及平台支持情况，不连接或修改运行实例。
- 增加 `pm doctor`：检查 daemon/IPC、权限、配置来源、进程树后端和平台能力，不执行修复操作。
- `list --json` 和 `inspect` 已使用跨平台公共 runtime snapshot，包含 generation、最后退出原因、
  ready/heartbeat、进程树后端与降级原因、有效配置来源以及完整重启状态；后续继续补充故障分类。
- 继续强化 IPC 权限：Linux/Android 已使用 `SO_PEERCRED` 和 UID/GID allowlist，Windows 已在每个 named pipe 实例应用 SDDL；后续补充部署身份检查和权限诊断输出。
- 形成 Linux/Android 可复现安装产物和 Windows MSVC 发布包，提供版本核对、升级前检查及回滚步骤。

进入下一阶段前必须满足：

- 崩溃循环不会造成无界、无间隔重启。
- 无效新配置不会中断当前正在运行的进程集合。
- Linux、Android、Windows 的公共字段和状态均有契约测试。
- 发布产物可在目标平台安装、核对版本并恢复到上一版本。

## 阶段二：健康与依赖恢复

目标是从“进程仍存在”提升到“服务实际可用”，并减少 reload 的业务扰动。

- 将 SDK 4.0 ready/tick 进一步归入统一 readiness/liveness 状态模型；当前已具备固定槽合并、持久连接、
  状态快照、可取消关闭和有界重连的低成本路径。
- 增加可选 exec、TCP、HTTP 健康检查；仅在配置启用时创建相关任务，不在核心中常驻 HTTP 服务。
- 依赖条件扩展为 `started`、`ready`、`healthy`，明确依赖失效后等待、停止和恢复策略。
- 将 reload 改为配置差异驱动：只停止和重启受影响进程，并按依赖图传播必要变更。
- 增加有界生命周期事件流，提供 JSON CLI 消费方式，覆盖启动、退出、超时、重启、降级和配置变更。
- 提供可选资源限制：Linux/Android 复用 cgroup，Windows 复用 Job Object，支持 CPU、内存和进程数限制及能力报告。

进入下一阶段前必须满足：

- 健康检查、依赖失效和恢复均有故障注入测试。
- reload 不重启配置及依赖均未变化的进程。
- 事件队列有固定上限、丢弃策略和丢弃计数。
- 资源限制无法启用时安全降级，并能通过状态和日志识别。

## 阶段三：可选运维扩展

目标是在不扩大核心攻击面和资源占用的前提下，提高批量设备运维能力。

- 提供诊断包导出，收集脱敏后的有效配置、版本、状态、近期事件和有界日志。
- 使用独立可选 sidecar 导出指标或接入远程运维；核心 daemon 不直接提供 HTTP 管理端口。
- 增加协议能力协商、配置迁移检查、发布清单、SBOM 和产物签名验证。
- 提供受限事件处理接口，用于设备告警和恢复动作；不在核心中加入通用脚本或插件运行时。

## 明确不做

- Node.js 专用 cluster 和内置网络负载均衡。
- Kubernetes、Docker Compose 等容器编排职责。
- 替代 systemd、Android init 或 Windows SCM。
- 在核心 daemon 中内置 Web UI、TUI、云监控或默认开放的远程 API。
- 通用 cron/工作流调度、MCP 服务以及不限能力的插件系统。

这些边界不是永久禁止独立工具或 sidecar，而是避免它们侵入 PM_Tiny 的可靠监督核心。

## 路线图维护规则

- 路线图按阶段和准入条件推进，不承诺未经评估的日期。
- 已实现能力应移入 README 或对应架构文档，路线图只保留尚未完成的方向。
- 每项新能力在实现前应明确资源成本、平台差异、失败模式、兼容策略和测试矩阵。
- 协议或配置不兼容变更必须记录迁移方式，不能只更新某个平台或某个调用方。

## 主要资料来源

- PM2 官方仓库及文档：[GitHub](https://github.com/Unitech/pm2)、[Documentation](https://pm2.keymetrics.io/docs/usage/quick-start/)
- Supervisor 官方仓库及文档：[GitHub](https://github.com/Supervisor/supervisor)、[Events](https://supervisord.org/events.html)
- systemd 官方站点及服务文档：[systemd.io](https://systemd.io/)、[systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)
- s6-overlay 与 runit：[s6-overlay](https://github.com/just-containers/s6-overlay)、[runit](https://smarden.org/runit/)
- process-compose 官方仓库及文档：[GitHub](https://github.com/F1bonacc1/process-compose)、[Documentation](https://f1bonacc1.github.io/process-compose/)
- WinSW 官方仓库：[GitHub](https://github.com/winsw/winsw)
