#ifndef PM_TINY_PROCESS_TREE_CONTROLLER_H
#define PM_TINY_PROCESS_TREE_CONTROLLER_H

#include "process_tree_types.h"

#include <memory>
#include <string>

namespace pm_tiny {

class process_tree_backend;
class cgroup_fs;

class process_tree_controller {
public:
    process_tree_controller();
    explicit process_tree_controller(std::shared_ptr<cgroup_fs> fs);
    ~process_tree_controller();
    bool initialize(process_tree_mode requested, const std::string &configured_root,
                    const std::string &instance_key, std::string &reason);
    bool attach(pid_t pid, process_tree_handle &handle, std::string &reason) const;
    int signal(const process_tree_handle &handle, int signo) const;
    bool contains(const process_tree_handle &handle, pid_t pid) const;
    bool empty(const process_tree_handle &handle) const;
    void cleanup(process_tree_handle &handle) const;
    process_tree_mode effective_mode() const { return effective_mode_; }
    const std::string &root() const { return root_; }
    bool degraded() const { return requested_mode_ == process_tree_mode::auto_detect &&
                                   effective_mode_ != process_tree_mode::cgroup; }
    const std::string &degradation_reason() const { return degradation_reason_; }
    static bool enable_subreaper(std::string &reason);

private:
    process_tree_mode effective_mode_ = process_tree_mode::process_group;
    process_tree_mode requested_mode_ = process_tree_mode::auto_detect;
    std::string root_;
    std::string degradation_reason_;
    std::unique_ptr<process_tree_backend> backend_;
    std::shared_ptr<cgroup_fs> cgroup_fs_;
};

const char *process_tree_mode_name(process_tree_mode mode);
bool parse_process_tree_mode(const std::string &value, process_tree_mode &mode);

} // namespace pm_tiny

#endif
