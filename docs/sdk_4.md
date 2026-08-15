# PM_Tiny SDK 4.0

PM_Tiny 4.0 提供 C++14 API 和版本化 C ABI，用于受管应用发送 ready/tick。SDK 只连接本机
Unix Domain Socket 或 Windows named pipe，不启动 daemon，也不读取或修改 daemon 配置。

## C++ API

```cpp
#include <pm_tiny_sdk.hpp>

pm_tiny::client client;
if (client.status().enabled) {
    client.ready();
    client.tick();
    client.flush(std::chrono::seconds(2));
}
```

`client_config.app_name` 和 `client_config.endpoint` 非空时优先使用显式值；缺失字段分别回落到 daemon
注入的 `PM_TINY_APP_NAME` 以及 Linux/Android 的 `PM_TINY_SOCK_FILE` 或 Windows 的
`PM_TINY_PIPE_NAME`。Linux/Android 的 `uds_abstract_namespace` 取值为 `-1` 时读取
`PM_TINY_UDS_ABSTRACT_NAMESPACE`。

`ready()` 和 `tick()` 返回：

- `queued`：对应槽从空变为待发送。
- `coalesced`：同类事件已经待发送，本次调用合并到该槽。
- `disabled`：应用名或 IPC 地址缺失，客户端不创建线程。
- `stopped`：客户端已经关闭。

每个启用客户端只有一个 worker 和 ready/tick 两个固定槽，不使用通用消息队列。ready 优先于 tick。
发送期间出现的新同类事件由序号保护，不会被旧发送完成错误清除。

连接在多次发送之间复用。连接或写入失败后保留 pending，从 200 ms 开始按 2 倍退避重连，最大 5 秒。
错误不会持续写 stderr，可通过 `status().last_error`、`reconnect_attempts` 和 `retry_delay_ms` 查询。

`flush(timeout)` 等待两个槽均发送完成；成功仅表示协议帧已经写入本地 IPC，不表示 daemon 已处理该帧。
析构或 `close()` 会立即丢弃 pending、取消平台 I/O 并等待 worker 退出。需要可靠退出时必须先显式调用
`flush(timeout)`。

## C ABI

```c
#include <pm_tiny_sdk.h>

pm_tiny_client_config_t config = {0};
config.struct_size = sizeof(config);
config.abi_version = PM_TINY_SDK_ABI_VERSION;
config.uds_abstract_namespace = -1;

pm_tiny_client_t *client = NULL;
if (pm_tiny_client_create(&config, &client) == 0) {
    pm_tiny_client_ready(client);
    pm_tiny_client_flush(client, 2000);
    pm_tiny_client_destroy(client);
}
```

所有公开配置和状态结构均包含 `struct_size` 与 `abi_version`。调用方必须将二者设置为当前头文件定义；
字符串配置在 create 时复制，状态中的应用名、端点和错误文本使用固定 256 字节缓冲区。

## 资源与线程

- 禁用客户端：不创建线程，不连接 IPC。
- 启用客户端：一个 worker，最多一个持久 IPC 连接，两个固定事件槽。
- SDK 不链接 YAML、daemon session、进程管理、日志模块或通用 Asio client。
- SDK 保持协议 v3 的 `0x31`/`0x32` wire layout，payload 仍为应用名称字符串。
