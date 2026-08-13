#pragma once

#include <string>

namespace pm_tiny {
namespace cli {

enum class connection_transport {
    unix_filesystem,
    unix_abstract,
    windows_named_pipe,
};

enum class connection_error_category {
    endpoint_missing,
    connection_refused,
    access_denied,
    endpoint_busy,
    other,
};

struct connection_error_info {
    std::string endpoint;
    connection_transport transport = connection_transport::unix_filesystem;
    std::string reason;
    std::string code_name;
    unsigned long code = 0;
    connection_error_category category = connection_error_category::other;
};

connection_error_category classify_posix_connection_error(int error_code);
connection_error_category classify_windows_connection_error(unsigned long error_code);
std::string format_connection_error(const connection_error_info &info);

} // namespace cli
} // namespace pm_tiny
