#pragma once

#include <string>
#include <vector>

#include "core/prog_cfg.h"

namespace pm_tiny {
namespace win {

struct ProgramConfig : prog_cfg_t {};

struct ConfigLoadResult {
    std::vector<ProgramConfig> programs;
    std::string error_message;
};

ConfigLoadResult load_program_configs(const std::string &program_config_path,
                                      const std::string &app_environ_dir);

} // namespace win
} // namespace pm_tiny
