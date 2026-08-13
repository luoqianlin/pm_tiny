#ifndef PM_TINY_CGROUP_V2_BACKEND_H
#define PM_TINY_CGROUP_V2_BACKEND_H

#include "process_tree_backend.h"
#include "cgroup_fs.h"

#include <memory>
#include <utility>

namespace pm_tiny {

class cgroup_v2_backend final : public process_tree_backend {
public:
    cgroup_v2_backend(std::string root, std::shared_ptr<cgroup_fs> fs)
        : root_(std::move(root)), fs_(std::move(fs)) {}
    process_tree_mode mode() const override { return process_tree_mode::cgroup; }
    bool attach(pid_t pid, process_tree_handle &handle, std::string &reason) const override;
    int signal(const process_tree_handle &handle, int signo) const override;
    bool empty(const process_tree_handle &handle) const override;
    void cleanup(process_tree_handle &handle) const override;

private:
    std::string root_;
    std::shared_ptr<cgroup_fs> fs_;
};

} // namespace pm_tiny

#endif
