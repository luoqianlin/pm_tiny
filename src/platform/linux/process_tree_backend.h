#ifndef PM_TINY_PROCESS_TREE_BACKEND_H
#define PM_TINY_PROCESS_TREE_BACKEND_H

#include "process_tree_types.h"

namespace pm_tiny {

class process_tree_backend {
public:
    virtual ~process_tree_backend() = default;
    virtual process_tree_mode mode() const = 0;
    virtual bool attach(pid_t pid, process_tree_handle &handle, std::string &reason) const = 0;
    virtual int signal(const process_tree_handle &handle, int signo) const = 0;
    virtual bool contains(const process_tree_handle &handle, pid_t pid) const = 0;
    virtual bool empty(const process_tree_handle &handle) const = 0;
    virtual void cleanup(process_tree_handle &handle) const = 0;
};

} // namespace pm_tiny

#endif
