#pragma once

#include "win_config_loader.h"

#include <string>
#include <vector>

namespace pm_tiny {
namespace win {

bool recover_program_config_save(const std::string &program_config_path,
                                 const std::string &app_environ_dir,
                                 std::string &error);

int save_program_configs(const std::vector<ProgramConfig> &configs,
                         const std::string &program_config_path,
                         const std::string &app_environ_dir,
                         std::string &error);

} // namespace win
} // namespace pm_tiny
