#include "process_group_backend.h"

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace pm_tiny {

bool process_group_backend::attach(pid_t pid, process_tree_handle &handle, std::string &) const {
    handle = process_tree_handle{};
    handle.mode = mode();
    handle.root_pid = pid;
    handle.pgid = pid;
    handle.active = true;
    return true;
}

int process_group_backend::signal(const process_tree_handle &handle, int signo) const {
    if (!handle.active || handle.pgid <= 0) return 0;
    return ::kill(-handle.pgid, signo);
}

bool process_group_backend::contains(const process_tree_handle &handle, pid_t pid) const {
    if (!handle.active || handle.pgid <= 0 || pid <= 0) return false;
    const pid_t pgid = ::getpgid(pid);
    return pgid > 0 && pgid == handle.pgid;
}

bool process_group_backend::empty(const process_tree_handle &handle) const {
    if (!handle.active || handle.pgid <= 0) return true;
    if (::kill(-handle.pgid, 0) == 0) return false;
    return errno == ESRCH;
}

void process_group_backend::cleanup(process_tree_handle &handle) const {
    handle = process_tree_handle{};
}

} // namespace pm_tiny
