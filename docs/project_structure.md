# 项目目录结构

PM_Tiny 按运行时代码、平台实现、测试职责和部署资产分离目录，避免将临时实验程序混入正式构建。

| 目录 | 内容 |
| --- | --- |
| `src/core` | 跨平台协议、配置、依赖图、状态、生命周期及日志公共代码 |
| `src/daemon` | Linux/Android daemon 与进程监督实现 |
| `src/cli` | Linux/Android CLI 及公共输出逻辑 |
| `src/platform` | Linux、Android、Windows 平台实现 |
| `sdk` | SDK 4.0 公共 C++/C 接口、共享状态机及 POSIX/Windows transport |
| `tests/unit` | 可由 CTest 独立执行的单元及组件测试 |
| `tests/integration/linux` | Linux CLI、协议、依赖和进程树集成测试 |
| `tests/integration/windows` | Windows named pipe、进程树和 SCM 集成测试 |
| `tests/fixtures` | 仅供集成测试启动的辅助进程和协议探针 |
| `tests/data` | Android/Windows 集成测试使用的 YAML 数据 |
| `examples/config` | 可复制修改的 Linux 和 Windows 配置样例 |
| `scripts/build` | 明确标注目标平台的构建入口 |
| `scripts/install` | 系统安装脚本 |
| `scripts/windows` | Windows SCM 管理脚本 |
| `packaging/systemd` | systemd 单元等打包资产 |
| `toolchains` | 嵌入式 Linux 交叉编译工具链 |
| `dependencies` | 固定版本、可离线构建的第三方源码 |

## 测试文件规则

- 自动化测试统一使用 `<subject>_test.cpp` 命名，并注册到 CTest。
- 不直接断言产品行为的辅助程序放入 `tests/fixtures`，名称使用 `_fixture` 或 `_probe` 后缀。
- 集成测试数据放入 `tests/data`，不得依赖开发者 home、固定 PID、个人绝对路径或生产 daemon。
- 标准库 API 示例、临时崩溃程序、手工终端输出实验和未被测试引用的辅助程序不进入仓库。
- 新增测试时应同时确认 Linux/Android 与 Windows 构建边界，平台专属测试放入对应目录。

## 构建入口

常规构建仍推荐直接使用 CMake。便捷脚本包括：

```bash
./scripts/build/build-linux.sh
PM_TINY_ANDROID_NDK=/path/to/ndk ./scripts/build/build-android.sh
./scripts/build/build-ax620a.sh
```

Windows 仅使用 VS 2022/MSVC x64，具体流程见
[`windows_port_status.md`](windows_port_status.md)。

## 日志模块边界

- `src/core/daemon_log.h/.cpp` 是 Linux、Android 和 Windows daemon 共用的诊断日志入口，负责级别、
  文本格式、并发串行化、轮转写入和控制台降级；平台目录只提供轮转文件的系统调用实现。
- `src/core/program_log.h/.cpp` 负责受管程序 stdout/stderr 的路径派生、内存尾部缓存和日志健康状态，
  不承担 daemon 自身诊断输出。
- daemon 日志尚未完成配置或日志文件打开、写入失败时会退回控制台，不能通过可空全局 logger
  或隐藏退出行为的宏改变 daemon 生命周期。
