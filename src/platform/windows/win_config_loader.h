#pragma once

#include <string>
#include <vector>

#include "core/prog_cfg.h"

namespace pm_tiny {
namespace win {

struct ProgramConfig : prog_cfg_t {
    std::string log_dir = "logs";
    std::string log_file_name;
    int log_max_size_kb = 4096;
    int log_file_count = 3;
};

struct ConfigLoadResult {
    std::vector<ProgramConfig> programs;
    std::string error_message;
};

ConfigLoadResult load_program_configs(const std::string &path);

} // namespace win
} // namespace pm_tiny
