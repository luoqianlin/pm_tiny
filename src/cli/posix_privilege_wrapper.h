#ifndef PM_TINY_POSIX_PRIVILEGE_WRAPPER_H
#define PM_TINY_POSIX_PRIVILEGE_WRAPPER_H

#include <string>

namespace pm_tiny {
namespace cli {

bool is_privilege_wrapper_executable(const std::string &executable);

} // namespace cli
} // namespace pm_tiny

#endif // PM_TINY_POSIX_PRIVILEGE_WRAPPER_H
