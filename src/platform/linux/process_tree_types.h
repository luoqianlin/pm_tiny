#ifndef PM_TINY_PROCESS_TREE_TYPES_H
#define PM_TINY_PROCESS_TREE_TYPES_H

#include <sys/types.h>
#include <string>

namespace pm_tiny {

enum class process_tree_mode {
    auto_detect,
    cgroup,
    process_group
};

struct process_tree_handle {
    process_tree_mode mode = process_tree_mode::process_group;
    pid_t root_pid = -1;
    pid_t pgid = -1;
    std::string cgroup_path;
    bool active = false;
};

} // namespace pm_tiny

#endif
