//
// Created by qianlinluo@foxmail.com on 24-7-31.
//
#include "pm_tiny_utility.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pm_tiny {
    int get_uid_by_pid(int pid) {
        char path[40];
        FILE *fp;
        char line[256];
        int uid = -1;

        // 构造文件路径
        snprintf(path, sizeof(path), "/proc/%d/status", pid);

        // 打开文件
        fp = fopen(path, "r");
        if (fp == NULL) {
            perror("Error opening file");
            return -1;
        }
        // 读取文件内容并寻找Uid行
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                sscanf(line, "Uid:\t%d", &uid);
                break;
            }
        }

        fclose(fp);
        return uid;
    }
}