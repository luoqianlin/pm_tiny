//
// Created by qianlinluo@foxmail.com on 23-9-7.
//
#include "../sdk/PM_Tiny_app_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>

int safe_sleep(long millsec) {
    struct timespec request, remain;
    int rc;
    request.tv_nsec = (millsec % 1000) * 1000000;
    request.tv_sec = millsec / 1000;
    memset(&remain, 0, sizeof(remain));
    do {
        errno = 0;
        rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain);
        if (rc == EINTR) {
            memcpy(&request, &remain, sizeof(remain));
        } else {
            break;
        }
    } while (1);
    return rc;
}

int main() {
    puts("===start===");
    PM_Tiny_Handle handle = PM_Tiny_Init();
    if (handle == NULL) {
        exit(EXIT_FAILURE);
    }
    puts("======");
    char app_name[200];
    PM_Tiny_get_app_name(handle, app_name, sizeof(app_name));
    printf("pm_tiny is enable:%d app name:%s\n", PM_Tiny_is_enable(handle), app_name);
    sleep(1);
    PM_Tiny_ready(handle);
    for (int i = 0; i < 10000; i++) {
        safe_sleep(100);
        if (i == 10) {
            printf("sleep 2\n");
            safe_sleep(2500);
        }
        PM_Tiny_tick(handle);
    }
    PM_Tiny_Destroy(handle);
    return 0;
}