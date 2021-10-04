//
// Created by qianlinluo@foxmail.com on 23-9-7.
//
#include "PM_Tiny_app_client.h"
#include "AppClient.h"
#include <string.h>

extern "C" {
PM_TINY_EXPORTS
PM_Tiny_Handle PM_Tiny_Init() {
    try {
        return new pm_tiny::AppClient;
    } catch (std::exception &ex) {
        fprintf(stderr, "%s eror:%s\n", __FUNCTION__, ex.what());
        return nullptr;
    }
}

PM_TINY_EXPORTS
int PM_Tiny_is_enable(PM_Tiny_Handle handle) {
    pm_tiny::AppClient *client = static_cast<pm_tiny::AppClient *>(handle);
    return client->is_enable();
}

PM_TINY_EXPORTS
void PM_Tiny_get_app_name(PM_Tiny_Handle handle, char *name, int len) {
    if (name == nullptr)return;
    pm_tiny::AppClient *client = static_cast<pm_tiny::AppClient *>(handle);
    auto app_name = client->get_app_name();
    strncpy(name, app_name.c_str(), len);
    if (app_name.length() >= static_cast<size_t>(len)) {
        name[len - 1] = 0;
    }
}

PM_TINY_EXPORTS
void PM_Tiny_tick(PM_Tiny_Handle handle) {
    pm_tiny::AppClient *client = static_cast<pm_tiny::AppClient *>(handle);
    client->tick();
}

PM_TINY_EXPORTS
void PM_Tiny_ready(PM_Tiny_Handle handle) {
    pm_tiny::AppClient *client = static_cast<pm_tiny::AppClient *>(handle);
    client->ready();
}

PM_TINY_EXPORTS
void PM_Tiny_Destroy(PM_Tiny_Handle handle) {
    pm_tiny::AppClient *client = static_cast<pm_tiny::AppClient *>(handle);
    delete client;
}
}