//
// Created by qianlinluo@foxmail.com on 2022/6/28.
//

#ifndef PM_TINY_LOG_H
#define PM_TINY_LOG_H
#include "logger.hpp"
#include <memory>

namespace pm_tiny{
    extern std::shared_ptr<logger_t> logger;
}

#define PM_TINY_LOG_E(fmt, ...)         pm_tiny::logger->error("[%20s] " fmt , __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_E_SYS(fmt, ...)     pm_tiny::logger->syscall_errorlog("[%20s] " fmt , __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_I(fmt, ...)         pm_tiny::logger->info("[%20s] " fmt , __func__, ##__VA_ARGS__)
#define PM_TINY_LOG_D(fmt, ...)         pm_tiny::logger->debug("[%20s] " fmt , __func__, ##__VA_ARGS__)

#endif //PM_TINY_LOG_H
