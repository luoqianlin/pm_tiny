#include "pm_tiny_sdk.h"
#include "pm_tiny_sdk.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>

struct pm_tiny_client {
    explicit pm_tiny_client(const pm_tiny::client_config &config) : value(config) {}
    pm_tiny::client value;
};

namespace {

template <std::size_t Size>
void copy_text(char (&destination)[Size], const std::string &source) {
    const std::size_t count = std::min(source.size(), Size - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

pm_tiny_enqueue_result_t convert(pm_tiny::enqueue_result result) {
    return static_cast<pm_tiny_enqueue_result_t>(static_cast<std::int32_t>(result));
}

} // namespace

extern "C" {

int32_t pm_tiny_client_create(const pm_tiny_client_config_t *config, pm_tiny_client_t **client) {
    if (client == nullptr) return -1;
    *client = nullptr;
    pm_tiny::client_config cpp_config;
    if (config != nullptr) {
        if (config->struct_size < sizeof(pm_tiny_client_config_t) ||
            config->abi_version != PM_TINY_SDK_ABI_VERSION ||
            config->uds_abstract_namespace < -1 || config->uds_abstract_namespace > 1) return -1;
        if (config->app_name != nullptr) cpp_config.app_name = config->app_name;
        if (config->endpoint != nullptr) cpp_config.endpoint = config->endpoint;
        cpp_config.uds_abstract_namespace = config->uds_abstract_namespace;
    }
    try {
        *client = new pm_tiny_client(cpp_config);
        return 0;
    } catch (...) {
        return -2;
    }
}

pm_tiny_enqueue_result_t pm_tiny_client_ready(pm_tiny_client_t *client) {
    return client == nullptr ? PM_TINY_ENQUEUE_INVALID_ARGUMENT : convert(client->value.ready());
}

pm_tiny_enqueue_result_t pm_tiny_client_tick(pm_tiny_client_t *client) {
    return client == nullptr ? PM_TINY_ENQUEUE_INVALID_ARGUMENT : convert(client->value.tick());
}

int32_t pm_tiny_client_flush(pm_tiny_client_t *client, uint32_t timeout_ms) {
    if (client == nullptr) return -1;
    return client->value.flush(std::chrono::milliseconds(timeout_ms)) ? 1 : 0;
}

int32_t pm_tiny_client_status(const pm_tiny_client_t *client, pm_tiny_client_status_t *status) {
    if (client == nullptr || status == nullptr ||
        status->struct_size < sizeof(pm_tiny_client_status_t) ||
        status->abi_version != PM_TINY_SDK_ABI_VERSION) return -1;
    const pm_tiny::client_status value = client->value.status();
    status->enabled = value.enabled;
    status->running = value.running;
    status->connected = value.connected;
    status->pending_ready = value.pending_ready;
    status->pending_tick = value.pending_tick;
    status->ready_sent = value.ready_sent;
    status->tick_sent = value.tick_sent;
    status->ready_coalesced = value.ready_coalesced;
    status->tick_coalesced = value.tick_coalesced;
    status->reconnect_attempts = value.reconnect_attempts;
    status->retry_delay_ms = value.retry_delay_ms;
    copy_text(status->app_name, value.app_name);
    copy_text(status->endpoint, value.endpoint);
    copy_text(status->last_error, value.last_error);
    return 0;
}

void pm_tiny_client_close(pm_tiny_client_t *client) {
    if (client != nullptr) client->value.close();
}

void pm_tiny_client_destroy(pm_tiny_client_t *client) {
    delete client;
}

} // extern "C"
