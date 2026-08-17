# 测试平台

PM_Tiny 的测试分为常规 CTest、真实操作系统能力、覆盖率、模糊测试和发布事务五层。常规构建不依赖
Clang、AFL++、root 权限或网络；专项任务显式安装并调用对应工具。

## 常规与真实运行场景

Linux 完整回归：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPM_TINY_BUILD_TESTS=ON
cmake --build build --parallel 4
cmake -E chdir build ctest --output-on-failure
```

`posix_runtime_integration` 使用真实 PTY 验证三个标准流的 tty 属性，并按 1 KiB 边界验证 split 日志
的 `.1`/`.2` 轮转顺序和淘汰。`cgroup_v2_integration` 在普通 CTest 无权限时返回 77，属于明确跳过，
不能作为真实通过证据。严格门禁使用：

```bash
PM_TINY_TEST_BIN="$PWD/build" PM_TINY_REQUIRE_REAL_CGROUP=1 \
PM_TINY_ALLOW_SUDO=1 bash tests/integration/linux/cgroup_v2_integration.sh
```

该测试创建唯一 cgroup，检查父子进程实际成员关系、完整树停止和 cgroup 目录回收。Windows 的对应
Release CTest 使用 VS 2022/MSVC x64，验证真实 Job Object、named pipe、SCM 和 split 日志轮转；
MinGW 结果不能替代该门禁。

## LLVM coverage

安装 Clang、`llvm-profdata` 和 `llvm-cov` 后运行：

```bash
bash scripts/test/coverage.sh
```

脚本建立独立的 `build-coverage`，以 `-fprofile-instr-generate -fcoverage-mapping` 编译并运行完整
CTest。产物位于 `build/test-artifacts/coverage/`：

- `coverage.profdata`：合并后的原始计数；
- `summary.txt`：文件和总计摘要；
- `html/`：可浏览的逐行报告；
- `coverage.lcov`：供 CI 或其他工具消费的 LCOV 数据。

首期只记录基线，不设覆盖率硬阈值。多个测试可执行文件会重复链接部分公共源码，LLVM 可能报告
同名函数的 profile hash 不同；汇总仍保留每个已插桩目标的数据，不把该提示误报为测试失败。

## libFuzzer 与 AFL++

三个目标分别覆盖协议分块/编解码、YAML 配置与依赖图、日志请求与 bounded tail：

```bash
# PR smoke，默认每目标 30 秒
bash scripts/test/fuzz.sh smoke libfuzzer
bash scripts/test/fuzz.sh smoke afl

# nightly，默认每目标 600 秒
bash scripts/test/fuzz.sh nightly libfuzzer
bash scripts/test/fuzz.sh nightly afl
```

libFuzzer 需要 Clang；AFL++ 需要 `afl-clang-fast`、`afl-clang-fast++` 和 `afl-fuzz`。两套构建均启用
ASan/UBSan，seed corpus 保持在 `tests/fuzz/corpus/`，运行时演化 corpus 和 crash 输入只写入
`build/test-artifacts/fuzz/`。专项构建默认关闭，不给普通二进制增加 sanitizer 或 fuzz 运行依赖。

## 安装、升级和回滚事务

发布目录使用 `releases/<release-id>`，manifest 记录软件版本、平台、架构、协议/配置 schema 以及每个
文件的大小和 SHA-256。配置、程序状态和日志始终位于 release 目录之外。Linux 示例：

```bash
scripts/release/deploy-linux.sh --root /srv/pm_tiny-release \
  --release-id 4.1.0-build1 --source-dir build
scripts/release/deploy-linux.sh --root /srv/pm_tiny-release \
  --action rollback --target-release-id 4.1.0-build1
```

Linux 通过同目录临时链接和 `rename` 原子替换 `current`。Windows 通过 `deploy-windows.ps1` 原子替换
`current.release`，并同步保存、更新和恢复 SCM BinaryPath。两端流程均为 staging、manifest/hash、
`pm_tiny --version`、隔离 daemon `info --json`、切换、切换后健康检查；切换前失败不改变 current，
切换后失败自动恢复，残留 journal 会在下一次操作前恢复。Windows 示例必须使用专用服务名：

```powershell
.\scripts\release\deploy-windows.ps1 -Root C:\PMTiny\releases -ReleaseId 4.1.0-build1 `
  -SourceDir C:\PMTiny\build\Release -ConfigPath C:\PMTiny\state\pm_tiny.yaml `
  -HomePath C:\PMTiny\state -PipeName '\\.\pipe\pm_tiny-release' -ServiceName pm_tiny_release
```

CTest 的 Linux/Windows release transaction 场景全部使用临时根、唯一 IPC 和唯一服务名，并注入
切换前失败、切换后失败和中断恢复，不访问 `/usr/local`、默认 pipe/socket 或生产 `pm_tiny` 服务。

## CI 与设备边界

`build.yml` 对每次 push/PR 运行 Linux、sanitizer、真实 cgroup、coverage、两套 30 秒 fuzz smoke、
Android arm64 构建和 Windows MSVC CTest，并上传 coverage、fuzz crash/corpus、Windows 测试现场。
`fuzz-nightly.yml` 每日运行两套每目标 600 秒任务。Android 继续使用现有三组显式 serial 的实机 smoke，
不在本阶段建设 Android 安装升级回滚，设备命令始终使用 `pm2`。
