#ifndef PM_TINY_CGROUP_FS_H
#define PM_TINY_CGROUP_FS_H

#include <sys/types.h>

#include <string>
#include <vector>

namespace pm_tiny {

class cgroup_fs {
public:
    virtual ~cgroup_fs() = default;
    virtual bool is_v2_available(const std::string &root) const = 0;
    virtual bool create_group(const std::string &path, std::string &reason) = 0;
    virtual bool attach_pid(const std::string &group, pid_t pid, std::string &reason) = 0;
    virtual bool collect_pids(const std::string &group, std::vector<pid_t> &pids,
                              std::string &reason) const = 0;
    virtual bool list_child_groups(const std::string &group, std::vector<std::string> &children,
                                   std::string &reason) const = 0;
    virtual bool remove_group(const std::string &path, std::string &reason) = 0;
    virtual int signal_pid(pid_t pid, int signo) = 0;
};

class posix_cgroup_fs final : public cgroup_fs {
public:
    bool is_v2_available(const std::string &root) const override;
    bool create_group(const std::string &path, std::string &reason) override;
    bool attach_pid(const std::string &group, pid_t pid, std::string &reason) override;
    bool collect_pids(const std::string &group, std::vector<pid_t> &pids,
                      std::string &reason) const override;
    bool list_child_groups(const std::string &group, std::vector<std::string> &children,
                           std::string &reason) const override;
    bool remove_group(const std::string &path, std::string &reason) override;
    int signal_pid(pid_t pid, int signo) override;
};

} // namespace pm_tiny

#endif
