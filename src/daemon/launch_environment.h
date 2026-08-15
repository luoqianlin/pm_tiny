#ifndef PM_TINY_LAUNCH_ENVIRONMENT_H
#define PM_TINY_LAUNCH_ENVIRONMENT_H

#include "pm_sys.h"

#include <string>
#include <vector>

namespace pm_tiny {

bool executable_has_path(const std::string &executable);

std::vector<std::string> compose_launch_environment(
        const std::vector<std::string> &inherited,
        const std::vector<std::string> &explicit_values,
        const passwd_t *target_user,
        bool sanitize_for_user_switch);

} // namespace pm_tiny

#endif // PM_TINY_LAUNCH_ENVIRONMENT_H
