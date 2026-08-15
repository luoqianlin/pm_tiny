#include "pm_tiny_sdk.h"

#include <stddef.h>

int main(void) {
    pm_tiny_client_config_t config = {0};
    pm_tiny_client_t *client = NULL;
    config.struct_size = sizeof(config);
    config.abi_version = PM_TINY_SDK_ABI_VERSION;
    config.uds_abstract_namespace = -1;
    if (pm_tiny_client_create(&config, &client) != 0) return 1;
    pm_tiny_client_destroy(client);
    return 0;
}
