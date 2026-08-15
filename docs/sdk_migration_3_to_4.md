# SDK 3.x 到 4.0 迁移

4.0 是破坏性升级，不提供旧 `AppClient`、`PM_Tiny_*` 符号或兼容头文件。

| 3.x | 4.0 |
| --- | --- |
| `#include "AppClient.h"` | `#include <pm_tiny_sdk.hpp>` |
| `pm_tiny::AppClient` | `pm_tiny::client` |
| `is_enable()` | `status().enabled` |
| `get_app_name()` | `status().app_name` |
| `ready()` / `tick()` 无返回值 | 返回 `enqueue_result` |
| 无可靠排空接口 | `flush(timeout)` |
| `PM_Tiny_Init` / `PM_Tiny_Destroy` | `pm_tiny_client_create` / `pm_tiny_client_destroy` |

迁移时应更新所有调用方并重新编译。不要根据头文件手工声明旧符号，也不要在新库外包一层无界重试；
SDK 已提供有界退避、合并和取消。进程退出前需要尽力送达最后一次 ready/tick 时，先调用
`flush(timeout)`，再调用 `close()` 或进入析构。
