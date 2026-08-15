//
// Created by qianlinluo@foxmail.com on 2022/6/28.
//

#ifndef PM_TINY_MEMORY_UTIL_H
#define PM_TINY_MEMORY_UTIL_H
#include <string>

namespace pm_tiny {
    namespace utils {
        namespace memory {
            std::string to_human_readable_size(long long kib);
        }
    }
};
#endif //PM_TINY_MEMORY_UTIL_H
