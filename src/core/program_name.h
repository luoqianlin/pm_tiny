#ifndef PM_TINY_PROGRAM_NAME_H
#define PM_TINY_PROGRAM_NAME_H

#include <string>

namespace pm_tiny {

constexpr std::size_t program_name_max_length = 128;

bool is_valid_program_name(const std::string &name);
std::string program_name_validation_error(const std::string &name);

} // namespace pm_tiny

#endif
