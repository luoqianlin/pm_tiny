//
// Created by qianlinluo@foxmail.com on 2022/6/28.
//
#include "memory_util.h"

namespace pm_tiny {
    namespace utils {
        namespace memory {
            std::string to_human_readable_size(long long kib) {
                char buff[1024];
                if (kib >= 1024 * 1024) {
                    snprintf(buff, sizeof(buff), "%5.2fGB", kib / (1024.0 * 1024.0));
                } else if (kib >= 1024) {
                    snprintf(buff, sizeof(buff), "%5.2fMB", kib / 1024.0);
                } else {
                    snprintf(buff, sizeof(buff), "%5.2fKB", kib * 1.0);
                }
                return buff;
            }
        }
    }
};
