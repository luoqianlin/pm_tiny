#ifndef PM_TINY_SDK_H
#define PM_TINY_SDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# if defined(PM_TINY_API_EXPORTS)
#  define PM_TINY_SDK_API __declspec(dllexport)
# elif defined(PM_TINY_API_IMPORTS)
#  define PM_TINY_SDK_API __declspec(dllimport)
# else
#  define PM_TINY_SDK_API
# endif
#elif defined(PM_TINY_API_EXPORTS)
# define PM_TINY_SDK_API __attribute__((visibility("default")))
#else
# define PM_TINY_SDK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PM_TINY_SDK_ABI_VERSION 4u
#define PM_TINY_SDK_TEXT_CAPACITY 256u

typedef struct pm_tiny_client pm_tiny_client_t;

typedef enum pm_tiny_enqueue_result {
    PM_TINY_ENQUEUE_QUEUED = 0,
    PM_TINY_ENQUEUE_COALESCED = 1,
    PM_TINY_ENQUEUE_DISABLED = 2,
    PM_TINY_ENQUEUE_STOPPED = 3,
    PM_TINY_ENQUEUE_INVALID_ARGUMENT = -1
} pm_tiny_enqueue_result_t;

typedef struct pm_tiny_client_config {
    size_t struct_size;
    uint32_t abi_version;
    const char *app_name;
    const char *endpoint;
    int32_t uds_abstract_namespace;
} pm_tiny_client_config_t;

typedef struct pm_tiny_client_status {
    size_t struct_size;
    uint32_t abi_version;
    int32_t enabled;
    int32_t running;
    int32_t connected;
    int32_t pending_ready;
    int32_t pending_tick;
    uint64_t ready_sent;
    uint64_t tick_sent;
    uint64_t ready_coalesced;
    uint64_t tick_coalesced;
    uint64_t reconnect_attempts;
    uint32_t retry_delay_ms;
    char app_name[PM_TINY_SDK_TEXT_CAPACITY];
    char endpoint[PM_TINY_SDK_TEXT_CAPACITY];
    char last_error[PM_TINY_SDK_TEXT_CAPACITY];
} pm_tiny_client_status_t;

PM_TINY_SDK_API int32_t pm_tiny_client_create(
        const pm_tiny_client_config_t *config, pm_tiny_client_t **client);
PM_TINY_SDK_API pm_tiny_enqueue_result_t pm_tiny_client_ready(pm_tiny_client_t *client);
PM_TINY_SDK_API pm_tiny_enqueue_result_t pm_tiny_client_tick(pm_tiny_client_t *client);
PM_TINY_SDK_API int32_t pm_tiny_client_flush(pm_tiny_client_t *client, uint32_t timeout_ms);
PM_TINY_SDK_API int32_t pm_tiny_client_status(
        const pm_tiny_client_t *client, pm_tiny_client_status_t *status);
PM_TINY_SDK_API void pm_tiny_client_close(pm_tiny_client_t *client);
PM_TINY_SDK_API void pm_tiny_client_destroy(pm_tiny_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
