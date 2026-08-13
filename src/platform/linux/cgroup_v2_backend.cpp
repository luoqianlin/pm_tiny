#include "cgroup_v2_backend.h"

#include <signal.h>

#include <cerrno>
#include <vector>

namespace pm_tiny {

bool cgroup_v2_backend::attach(pid_t pid, process_tree_handle &handle, std::string &reason) const {
    handle = process_tree_handle{};
    handle.mode = mode();
    handle.root_pid = pid;
    handle.pgid = pid;
    handle.active = true;
    handle.cgroup_path = root_ + "/app-" + std::to_string(static_cast<long long>(pid));
    if (!fs_->create_group(handle.cgroup_path, reason)) {
        handle = process_tree_handle{};
        return false;
    }
    if (!fs_->attach_pid(handle.cgroup_path, pid, reason)) {
        std::string cleanup_reason;
        fs_->remove_group(handle.cgroup_path, cleanup_reason);
        handle = process_tree_handle{};
        return false;
    }
    return true;
}

int cgroup_v2_backend::signal(const process_tree_handle &handle, int signo) const {
    if (!handle.active) return 0;
    std::vector<pid_t> pids;
    std::string reason;
    if (!fs_->collect_pids(handle.cgroup_path, pids, reason)) {
        errno = EIO;
        return -1;
    }
    int rc = 0;
    int saved_error = 0;
    for (pid_t pid : pids) {
        if (fs_->signal_pid(pid, signo) != 0 && errno != ESRCH) {
            rc = -1;
            if (saved_error == 0) saved_error = errno;
        }
    }
    if (saved_error != 0) errno = saved_error;
    return rc;
}

bool cgroup_v2_backend::empty(const process_tree_handle &handle) const {
    if (!handle.active) return true;
    std::vector<pid_t> pids;
    std::string reason;
    if (!fs_->collect_pids(handle.cgroup_path, pids, reason)) return false;
    return pids.empty();
}

void cgroup_v2_backend::cleanup(process_tree_handle &handle) const {
    if (!handle.cgroup_path.empty()) {
        std::vector<pid_t> pids;
        std::string reason;
        if (fs_->collect_pids(handle.cgroup_path, pids, reason) && pids.empty()) {
            fs_->remove_group(handle.cgroup_path, reason);
        }
    }
    handle = process_tree_handle{};
}

} // namespace pm_tiny
