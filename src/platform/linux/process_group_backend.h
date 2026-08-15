#ifndef PM_TINY_PROCESS_GROUP_BACKEND_H
#define PM_TINY_PROCESS_GROUP_BACKEND_H

#include "process_tree_backend.h"

namespace pm_tiny {

class process_group_backend final : public process_tree_backend {
public:
    process_tree_mode mode() const override { return process_tree_mode::process_group; }
    bool attach(pid_t pid, process_tree_handle &handle, std::string &reason) const override;
    int signal(const process_tree_handle &handle, int signo) const override;
    bool contains(const process_tree_handle &handle, pid_t pid) const override;
    bool empty(const process_tree_handle &handle) const override;
    void cleanup(process_tree_handle &handle) const override;
};

} // namespace pm_tiny

#endif
