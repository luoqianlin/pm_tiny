#include "fuzz_adapters.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
#if defined(PM_TINY_FUZZ_protocol)
    pm_tiny::fuzz::protocol_input(data, size);
#elif defined(PM_TINY_FUZZ_config)
    pm_tiny::fuzz::config_input(data, size);
#elif defined(PM_TINY_FUZZ_log)
    pm_tiny::fuzz::log_input(data, size);
#else
#error "missing fuzz subject"
#endif
    return 0;
}
