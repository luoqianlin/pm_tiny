#include "process_tree_controller.h"
#include "cgroup_fs.h"
#include "cgroup_v2_backend.h"
#include "process_group_backend.h"

#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace pm_tiny {
namespace {

std::string sanitize(const std::string &value) {
    std::string result;
    for (char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            result.push_back(c);
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) result = "app";
    if (result.size() > 48) result.resize(48);
    return result;
}

} // namespace

process_tree_controller::process_tree_controller()
    : process_tree_controller(std::make_shared<posix_cgroup_fs>()) {}
process_tree_controller::process_tree_controller(std::shared_ptr<cgroup_fs> fs)
    : cgroup_fs_(fs ? std::move(fs) : std::make_shared<posix_cgroup_fs>()) {}
process_tree_controller::~process_tree_controller() = default;

const char *process_tree_mode_name(process_tree_mode mode) {
    switch (mode) {
        case process_tree_mode::auto_detect: return "auto";
        case process_tree_mode::cgroup: return "cgroup";
        case process_tree_mode::process_group: return "process_group";
    }
    return "process_group";
}

bool parse_process_tree_mode(const std::string &value, process_tree_mode &mode) {
    if (value == "auto") mode = process_tree_mode::auto_detect;
    else if (value == "cgroup" || value == "cgroup_v2") mode = process_tree_mode::cgroup;
    else if (value == "process_group") mode = process_tree_mode::process_group;
    else return false;
    return true;
}

bool process_tree_controller::enable_subreaper(std::string &reason) {
#ifdef PR_SET_CHILD_SUBREAPER
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1L) != 0) {
        reason = strerror(errno);
        return false;
    }
    int value = 0;
    if (::prctl(PR_GET_CHILD_SUBREAPER, &value) != 0 || value != 1) {
        reason = strerror(errno);
        return false;
    }
    return true;
#else
    reason = "PR_SET_CHILD_SUBREAPER is unavailable";
    return false;
#endif
}

bool process_tree_controller::initialize(process_tree_mode requested,
                                          const std::string &configured_root,
                                          const std::string &instance_key,
                                          std::string &reason) {
    reason.clear();
    requested_mode_ = requested;
    effective_mode_ = requested;
    root_.clear();
    degradation_reason_.clear();
    backend_.reset();
    if (requested == process_tree_mode::process_group) {
        backend_ = std::make_unique<process_group_backend>();
        return true;
    }

    std::string base = configured_root;
    if (base.empty()) base = "/sys/fs/cgroup";
    if (!cgroup_fs_->is_v2_available(base)) {
        reason = "cgroup v2 cgroup.procs is unavailable";
        if (requested == process_tree_mode::cgroup) return false;
        effective_mode_ = process_tree_mode::process_group;
        degradation_reason_ = reason;
        backend_ = std::make_unique<process_group_backend>();
        return true;
    }
    root_ = base + "/pm_tiny-" + sanitize(instance_key);
    if (!cgroup_fs_->create_group(root_, reason)) {
        if (requested == process_tree_mode::cgroup) return false;
        effective_mode_ = process_tree_mode::process_group;
        degradation_reason_ = reason;
        root_.clear();
        backend_ = std::make_unique<process_group_backend>();
        return true;
    }
    const std::string probe = root_ + "/.probe";
    bool probe_ok = cgroup_fs_->create_group(probe, reason);
    pid_t probe_pid = -1;
    if (probe_ok) {
        probe_pid = ::fork();
        if (probe_pid == 0) {
            for (;;) ::pause();
        }
        probe_ok = probe_pid > 0 && cgroup_fs_->attach_pid(probe, probe_pid, reason);
    }
    if (probe_pid > 0) {
        ::kill(probe_pid, SIGKILL);
        while (::waitpid(probe_pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    std::string cleanup_reason;
    cgroup_fs_->remove_group(probe, cleanup_reason);
    if (!probe_ok) {
        if (reason.empty()) reason = strerror(errno);
        reason = std::string("cgroup attach probe failed: ") + reason;
        cgroup_fs_->remove_group(root_, cleanup_reason);
        root_.clear();
        if (requested == process_tree_mode::cgroup) return false;
        effective_mode_ = process_tree_mode::process_group;
        degradation_reason_ = reason;
        backend_ = std::make_unique<process_group_backend>();
        return true;
    }
    effective_mode_ = process_tree_mode::cgroup;
    std::vector<std::string> stale_groups;
    if (cgroup_fs_->list_child_groups(root_, stale_groups, cleanup_reason)) {
        for (const auto &stale : stale_groups) {
            auto name_pos = stale.find_last_of('/');
            std::string name = name_pos == std::string::npos ? stale : stale.substr(name_pos + 1);
            if (name.rfind("app-", 0) != 0) continue;
            std::vector<pid_t> pids;
            if (!cgroup_fs_->collect_pids(stale, pids, cleanup_reason)) continue;
            for (pid_t pid : pids) cgroup_fs_->signal_pid(pid, SIGKILL);
            pids.clear();
            if (cgroup_fs_->collect_pids(stale, pids, cleanup_reason) && pids.empty()) {
                cgroup_fs_->remove_group(stale, cleanup_reason);
            }
        }
    }
    backend_ = std::make_unique<cgroup_v2_backend>(root_, cgroup_fs_);
    return true;
}

bool process_tree_controller::attach(pid_t pid, process_tree_handle &handle,
                                      std::string &reason) const {
    if (!backend_) {
        reason = "process tree controller is not initialized";
        return false;
    }
    return backend_->attach(pid, handle, reason);
}

int process_tree_controller::signal(const process_tree_handle &handle, int signo) const {
    return backend_ ? backend_->signal(handle, signo) : 0;
}

bool process_tree_controller::empty(const process_tree_handle &handle) const {
    return !backend_ || backend_->empty(handle);
}

void process_tree_controller::cleanup(process_tree_handle &handle) const {
    if (backend_) backend_->cleanup(handle);
    else handle = process_tree_handle{};
}

} // namespace pm_tiny
