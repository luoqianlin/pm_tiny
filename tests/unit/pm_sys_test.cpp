#include "core/pm_sys.h"
#include "core/globals.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "pm_sys_test failure: %s (errno=%d: %s)\n",
                 message, errno, std::strerror(errno));
    std::abort();
}

void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

std::array<int, 2> make_pipe() {
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) != 0) {
        fail("pipe failed");
    }
    return fds;
}

std::array<int, 2> make_socket_pair() {
    std::array<int, 2> fds{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        fail("socketpair failed");
    }
    return fds;
}

std::string make_temp_dir(const std::string &prefix) {
    std::string templ = prefix + "XXXXXX";
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    char *path = ::mkdtemp(buffer.data());
    expect(path != nullptr, "mkdtemp failed");
    return std::string(path);
}

void test_safe_read_write() {
    auto pipe_fds = make_pipe();
    const char *msg = "hello";
    auto written = pm_tiny::safe_write(pipe_fds[1], msg, std::strlen(msg));
    expect(written == static_cast<ssize_t>(std::strlen(msg)), "safe_write returned unexpected size");
    std::array<char, 6> buffer{};
    auto read_bytes = pm_tiny::safe_read(pipe_fds[0], buffer.data(), buffer.size());
    expect(read_bytes == static_cast<ssize_t>(std::strlen(msg)), "safe_read returned unexpected size");
    expect(std::string(buffer.data(), static_cast<size_t>(read_bytes)) == "hello",
           "safe_read content mismatch");
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

void test_safe_send() {
    auto sockets = make_socket_pair();
    const char *payload = "ping";
    auto sent = pm_tiny::safe_send(sockets[0], payload, std::strlen(payload), MSG_NOSIGNAL);
    expect(sent == static_cast<ssize_t>(std::strlen(payload)), "safe_send returned unexpected size");
    std::array<char, 4> buffer{};
    auto read_bytes = pm_tiny::safe_read(sockets[1], buffer.data(), buffer.size());
    expect(read_bytes == static_cast<ssize_t>(std::strlen(payload)), "safe_send content mismatch");
    expect(std::string(buffer.data(), static_cast<size_t>(read_bytes)) == "ping",
           "safe_send payload mismatch");
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_set_nonblock_and_cloexec() {
    auto pipe_fds = make_pipe();
    expect(pm_tiny::set_nonblock(pipe_fds[0]) == 0, "set_nonblock failed");
    int flags = fcntl(pipe_fds[0], F_GETFL);
    expect(flags & O_NONBLOCK, "O_NONBLOCK not set");
    expect(pm_tiny::set_cloexec(pipe_fds[0]) == 0, "set_cloexec failed");
    int fd_flags = fcntl(pipe_fds[0], F_GETFD);
    expect(fd_flags & FD_CLOEXEC, "FD_CLOEXEC not set");
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}

void test_is_directory_exists() {
    auto tmp_dir = make_temp_dir("/tmp/pm_sys_test_");
    expect(pm_tiny::is_directory_exists(tmp_dir.c_str()) == 1, "directory should exist");
    expect(pm_tiny::is_directory_exists((tmp_dir + "_missing").c_str()) == 0,
           "non-existent directory should not exist");
}

void test_get_vm_rss_kib() {
    auto rss = pm_tiny::get_vm_rss_kib(static_cast<int>(::getpid()));
    expect(rss >= 0, "VmRSS should be non-negative");
}

void test_passwd_lookup() {
    pm_tiny::passwd_t by_uid;
    expect(pm_tiny::get_user_from_uid(::getuid(), by_uid) == 0, "current uid lookup failed");
    expect(by_uid.pw_uid == ::getuid() && !by_uid.pw_name.empty(), "uid lookup returned wrong account");

    pm_tiny::passwd_t by_name;
    expect(pm_tiny::get_uid_from_username(by_uid.pw_name.c_str(), by_name) == 0,
           "current username lookup failed");
    expect(by_name.pw_uid == by_uid.pw_uid && by_name.pw_gid == by_uid.pw_gid,
           "username lookup returned wrong account");

    errno = 0;
    expect(pm_tiny::get_uid_from_username("pm_tiny_account_that_must_not_exist", by_name) == -1,
           "missing username should fail");
    expect(errno == ENOENT, "missing username should set ENOENT");
}

void test_process_liveness_and_wait() {
    expect(pm_tiny::is_process_exists(static_cast<int>(::getpid())) == 1,
           "current process should be running");
    expect(pm_tiny::is_process_exists(-1) == 0, "invalid pid should not exist");

    pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) _exit(0);
    for (int i = 0; i < 100; ++i) {
        char state = '\0';
        char path[64] = {0};
        std::snprintf(path, sizeof(path), "/proc/%d/stat", child);
        FILE *file = std::fopen(path, "r");
        char line[512] = {0};
        if (file != nullptr) {
            if (std::fgets(line, sizeof(line), file) != nullptr) {
                const char *end = std::strrchr(line, ')');
                if (end != nullptr && end[1] == ' ') state = end[2];
            }
            std::fclose(file);
        }
        if (state == 'Z') break;
        usleep(1000);
    }
    expect(pm_tiny::is_process_exists(child) == 0, "zombie should be treated as exited");
    expect(pm_tiny::wait_for_process_exit(child, 100) == pm_tiny::process_wait_result::exited,
           "zombie wait should finish");
    expect(::waitpid(child, nullptr, 0) == child, "waitpid failed");

    child = ::fork();
    expect(child >= 0, "second fork failed");
    if (child == 0) {
        for (;;) pause();
    }
    expect(pm_tiny::wait_for_process_exit(child, 20, 5) == pm_tiny::process_wait_result::timed_out,
           "running process wait should time out");
    expect(pm_tiny::wait_for_process_exit(child, 100, 5, []() { return true; }) ==
                   pm_tiny::process_wait_result::interrupted,
           "wait should report interruption");
    expect(::kill(child, SIGKILL) == 0, "child kill failed");
    expect(::waitpid(child, nullptr, 0) == child, "second waitpid failed");
}

void write_process_stat(const std::string &root, int pid, const std::string &content) {
    const std::string process_dir = root + "/" + std::to_string(pid);
    expect(::mkdir(process_dir.c_str(), 0700) == 0 || errno == EEXIST,
           "process stat directory creation failed");
    const std::string path = process_dir + "/stat";
    FILE *file = std::fopen(path.c_str(), "w");
    expect(file != nullptr, "process stat open failed");
    expect(std::fputs(content.c_str(), file) >= 0, "process stat write failed");
    expect(std::fclose(file) == 0, "process stat close failed");
}

void test_mock_process_states() {
    const std::string root = make_temp_dir("/tmp/pm_sys_proc_");
    char *original_procdir = procdir_path;
    procdir_path = const_cast<char *>(root.c_str());
    const int pid = static_cast<int>(::getpid());

    write_process_stat(root, pid, std::to_string(pid) + " (name with ) parenthesis) S 1 2 3\n");
    expect(pm_tiny::is_process_exists(pid) == 1, "running stat should exist");
    for (char state : {'Z', 'X', 'x'}) {
        write_process_stat(root, pid,
                           std::to_string(pid) + " (name with ) parenthesis) " + state + " 1 2 3\n");
        expect(pm_tiny::is_process_exists(pid) == 0, "terminal stat should be exited");
    }
    write_process_stat(root, pid, "invalid stat\n");
    expect(pm_tiny::is_process_exists(pid) == 1, "invalid stat should conservatively exist");
    procdir_path = original_procdir;
}

} // namespace

int main() {
    test_safe_read_write();
    test_safe_send();
    test_set_nonblock_and_cloexec();
    test_is_directory_exists();
    test_get_vm_rss_kib();
    test_passwd_lookup();
    test_process_liveness_and_wait();
    test_mock_process_states();
    return 0;
}
