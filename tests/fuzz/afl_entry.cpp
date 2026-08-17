#include "fuzz_adapters.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    std::vector<std::uint8_t> input;
    if (argc == 2) {
        std::ifstream stream(argv[1], std::ios::binary);
        input.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    } else {
        input.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    if (input.size() > 4U * 1024U * 1024U) return 0;
#if defined(PM_TINY_FUZZ_protocol)
    pm_tiny::fuzz::protocol_input(input.data(), input.size());
#elif defined(PM_TINY_FUZZ_config)
    pm_tiny::fuzz::config_input(input.data(), input.size());
#elif defined(PM_TINY_FUZZ_log)
    pm_tiny::fuzz::log_input(input.data(), input.size());
#else
#error "missing fuzz subject"
#endif
    return 0;
}
