#include "connection_error.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "connection_error_test failure: " << message << '\n';
        std::abort();
    }
}

void expect_contains(const std::string &text, const std::string &expected, const char *message) {
    expect(text.find(expected) != std::string::npos, message);
}

} // namespace

int main() {
    using namespace pm_tiny::cli;
    expect(classify_posix_connection_error(ENOENT) == connection_error_category::endpoint_missing,
           "ENOENT classification");
    expect(classify_posix_connection_error(ECONNREFUSED) == connection_error_category::connection_refused,
           "ECONNREFUSED classification");
    expect(classify_posix_connection_error(EACCES) == connection_error_category::access_denied,
           "EACCES classification");
    expect(classify_windows_connection_error(2) == connection_error_category::endpoint_missing,
           "ERROR_FILE_NOT_FOUND classification");
    expect(classify_windows_connection_error(5) == connection_error_category::access_denied,
           "ERROR_ACCESS_DENIED classification");
    expect(classify_windows_connection_error(231) == connection_error_category::endpoint_busy,
           "ERROR_PIPE_BUSY classification");

    connection_error_info info;
    info.endpoint = "socket\nname";
    info.transport = connection_transport::unix_abstract;
    info.reason = "No such file\r or directory";
    info.code_name = "errno";
    info.code = ENOENT;
    info.category = connection_error_category::endpoint_missing;
    const auto output = format_connection_error(info);
    expect_contains(output, "endpoint: @socket?name", "abstract endpoint formatting");
    expect_contains(output, "transport: unix socket (abstract)", "transport formatting");
    expect_contains(output, "reason: No such file? or directory (errno=", "reason formatting");
    expect_contains(output, "PM_TINY_SOCK_FILE and PM_TINY_UDS_ABSTRACT_NAMESPACE",
                    "Unix socket configuration hint");

    info.endpoint = "\\\\.\\pipe\\pm_tiny";
    info.transport = connection_transport::windows_named_pipe;
    info.reason = "The system cannot find the file specified";
    info.code_name = "winerror";
    info.code = 2;
    info.category = connection_error_category::endpoint_missing;
    const auto windows_output = format_connection_error(info);
    expect_contains(windows_output, "transport: Windows named pipe", "Windows transport formatting");
    expect_contains(windows_output, "PM_TINY_PIPE_NAME", "Windows pipe configuration hint");
    return 0;
}
