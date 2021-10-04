//
// Created by qianlinluo@foxmail.com on 2022/6/28.
//

#ifndef PM_TINY_LOG_H
#define PM_TINY_LOG_H
#include "logger.hpp"
#include "strtools.h"
#include <memory>

namespace pm_tiny{
    extern std::unique_ptr<logger_t> logger;
    int initialize();
}

#define PM_TINY_LOG_E(fmt, ...)         pm_tiny::logger->error("[%s:%d %s] " fmt ,PM_TINY_SHORT_FILE,__LINE__, __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_E_SYS(fmt, ...)     pm_tiny::logger->syscall_errorlog("[%s:%d %s] " fmt ,PM_TINY_SHORT_FILE,__LINE__, __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_I(fmt, ...)         pm_tiny::logger->info("[%s:%d %s] " fmt , PM_TINY_SHORT_FILE,__LINE__,__func__, ##__VA_ARGS__)
#define PM_TINY_LOG_D(fmt, ...)         pm_tiny::logger->debug("[%s:%d %s] " fmt ,PM_TINY_SHORT_FILE,__LINE__, __func__, ##__VA_ARGS__)

#define PM_TINY_LOG_FATAL(fmt, ...)         pm_tiny::logger->fatal("[%s:%d %s] " fmt ,PM_TINY_SHORT_FILE,__LINE__, __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_FATAL_SYS(fmt, ...)     pm_tiny::logger->syscall_fatal("[%s:%d %s] " fmt ,PM_TINY_SHORT_FILE,__LINE__, __func__, ##__VA_ARGS__)


#define PM_TINY_THROW(fmt, ...)  do{char _buf__[1024]; \
snprintf(_buf__, sizeof(_buf__), "[%s:%d] " fmt , PM_TINY_SHORT_FILE, __LINE__, ##__VA_ARGS__); \
throw std::runtime_error(_buf__);}while(0)

#endif //PM_TINY_LOG_H
