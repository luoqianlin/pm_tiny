#include "log.h"

namespace pm_tiny{
    std::unique_ptr<logger_t> logger;

    int initialize() {
        logger = std::make_unique<pm_tiny::logger_t>();
        return 0;
    }
}