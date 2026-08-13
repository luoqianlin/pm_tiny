#ifndef PM_TINY_PROCESS_REAPER_H
#define PM_TINY_PROCESS_REAPER_H

#include <sys/types.h>
#include <string>
#include <vector>

namespace pm_tiny {

struct reaped_child {
    pid_t pid = -1;
    int status = 0;
};

class process_reaper {
public:
    std::vector<reaped_child> reap_all() const;
};

std::string describe_reaped_descendant(const reaped_child &child);

} // namespace pm_tiny

#endif
