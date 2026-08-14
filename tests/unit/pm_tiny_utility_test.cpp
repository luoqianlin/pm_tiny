#include "core/pm_tiny_utility.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>
#include <unistd.h>

namespace {

[[noreturn]] void fail(const char *message) {
    std::cerr << "pm_tiny_utility_test failure: " << message
              << " (errno=" << errno << ": " << std::strerror(errno) << ")\n";
    std::abort();
}

void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

void test_closeable_fd_raii() {
    int pipe_fds[2];
    int rc = ::pipe(pipe_fds);
    expect(rc == 0, "pipe failed");
    {
        pm_tiny::CloseableFd read_fd(pipe_fds[0]);
        expect(static_cast<bool>(read_fd), "CloseableFd should hold descriptor");
        pm_tiny::CloseableFd moved_fd(std::move(read_fd));
        expect(!read_fd, "moved-from CloseableFd should be empty");
        expect(static_cast<bool>(moved_fd), "moved CloseableFd should hold descriptor");
    }
    char buffer{};
    errno = 0;
    ssize_t read_rc = ::read(pipe_fds[0], &buffer, 1);
    expect(read_rc == -1, "read should fail after descriptor closed");
    expect(errno == EBADF, "read should fail with EBADF");
    ::close(pipe_fds[1]);
}

void test_get_uid_by_pid() {
    int pid = static_cast<int>(::getpid());
    int uid = pm_tiny::get_uid_by_pid(pid);
    expect(uid == static_cast<int>(::getuid()), "uid lookup mismatch");
}

} // namespace

int main() {
    test_closeable_fd_raii();
    test_get_uid_by_pid();
    return 0;
}
