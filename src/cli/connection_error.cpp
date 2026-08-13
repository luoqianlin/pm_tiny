#include "connection_error.h"

#include <cerrno>
#include <sstream>

namespace pm_tiny {
namespace cli {
namespace {

std::string sanitize_line(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char ch : text) {
        result.push_back(ch < 0x20 || ch == 0x7f ? '?' : static_cast<char>(ch));
    }
    return result;
}

const char *transport_name(connection_transport transport) {
    switch (transport) {
        case connection_transport::unix_filesystem: return "unix socket (filesystem)";
        case connection_transport::unix_abstract: return "unix socket (abstract)";
        case connection_transport::windows_named_pipe: return "Windows named pipe";
    }
    return "unknown";
}

const char *connection_hint(connection_transport transport,
                            connection_error_category category) {
    const bool windows = transport == connection_transport::windows_named_pipe;
    switch (category) {
        case connection_error_category::endpoint_missing:
            return windows
                ? "start pm_tiny or verify PM_TINY_PIPE_NAME matches the daemon."
                : "start pm_tiny or verify PM_TINY_SOCK_FILE and PM_TINY_UDS_ABSTRACT_NAMESPACE match the daemon.";
        case connection_error_category::connection_refused:
            return "restart pm_tiny and remove any stale socket only after confirming no daemon is using it.";
        case connection_error_category::access_denied:
            return "verify the current user has permission to access the pm_tiny endpoint.";
        case connection_error_category::endpoint_busy:
            return "retry later or verify the pm_tiny control pipe is accepting clients.";
        case connection_error_category::other:
            return windows
                ? "verify pm_tiny is running and PM_TINY_PIPE_NAME matches the daemon."
                : "verify pm_tiny is running and the Unix socket settings match the daemon.";
    }
    return "verify the pm_tiny connection configuration.";
}

} // namespace

connection_error_category classify_posix_connection_error(int error_code) {
    switch (error_code) {
        case ENOENT: return connection_error_category::endpoint_missing;
        case ECONNREFUSED: return connection_error_category::connection_refused;
        case EACCES: return connection_error_category::access_denied;
        default: return connection_error_category::other;
    }
}

connection_error_category classify_windows_connection_error(unsigned long error_code) {
    // Win32 error values are stable and kept here so the formatter remains portable.
    switch (error_code) {
        case 2: return connection_error_category::endpoint_missing; // ERROR_FILE_NOT_FOUND
        case 5: return connection_error_category::access_denied; // ERROR_ACCESS_DENIED
        case 231: return connection_error_category::endpoint_busy; // ERROR_PIPE_BUSY
        default: return connection_error_category::other;
    }
}

std::string format_connection_error(const connection_error_info &info) {
    std::string endpoint = sanitize_line(info.endpoint);
    if (info.transport == connection_transport::unix_abstract &&
        (endpoint.empty() || endpoint.front() != '@')) {
        endpoint.insert(endpoint.begin(), '@');
    }

    std::ostringstream output;
    output << "pm: cannot connect to pm_tiny\n"
           << "  endpoint: " << endpoint << "\n"
           << "  transport: " << transport_name(info.transport) << "\n"
           << "  reason: " << sanitize_line(info.reason);
    if (!info.code_name.empty()) {
        output << " (" << sanitize_line(info.code_name) << '=' << info.code << ')';
    }
    output << "\n  hint: " << connection_hint(info.transport, info.category) << '\n';
    return output.str();
}

} // namespace cli
} // namespace pm_tiny
