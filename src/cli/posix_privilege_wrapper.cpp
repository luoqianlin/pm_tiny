#include "posix_privilege_wrapper.h"

namespace pm_tiny {
namespace cli {

bool is_privilege_wrapper_executable(const std::string &executable) {
    const auto separator = executable.find_last_of('/');
    const auto basename = separator == std::string::npos ? executable : executable.substr(separator + 1);
    return basename == "sudo" || basename == "su" || basename == "doas";
}

} // namespace cli
} // namespace pm_tiny
