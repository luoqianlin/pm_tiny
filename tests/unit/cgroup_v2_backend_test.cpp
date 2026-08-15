#include "platform/linux/cgroup_v2_backend.h"
#include "platform/linux/process_tree_controller.h"

#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <set>
#include <signal.h>
#include <utility>

class fake_cgroup_fs final : public pm_tiny::cgroup_fs {
public:
    bool fail_create = false;
    bool fail_attach = false;
    bool fail_collect = false;
    bool available = true;
    std::map<std::string, std::vector<pid_t>> groups;
    std::vector<std::pair<pid_t, int>> signals;
    std::set<std::string> removed;

    bool is_v2_available(const std::string &) const override { return available; }
    bool create_group(const std::string &path, std::string &reason) override {
        if (fail_create) {
            reason = "create failed";
            return false;
        }
        groups[path];
        return true;
    }
    bool attach_pid(const std::string &group, pid_t pid, std::string &reason) override {
        if (fail_attach) {
            reason = "attach failed";
            return false;
        }
        groups[group].push_back(pid);
        return true;
    }
    bool collect_pids(const std::string &group, std::vector<pid_t> &pids,
                      std::string &reason) const override {
        if (fail_collect) {
            reason = "read failed";
            return false;
        }
        for (const auto &entry : groups) {
            const auto &path = entry.first;
            const auto &members = entry.second;
            if (path == group || (path.size() > group.size() &&
                                  path.compare(0, group.size(), group) == 0 &&
                                  path[group.size()] == '/')) {
                pids.insert(pids.end(), members.begin(), members.end());
            }
        }
        return true;
    }
    bool list_child_groups(const std::string &group, std::vector<std::string> &children,
                           std::string &) const override {
        const std::string prefix = group + "/";
        for (const auto &entry : groups) {
            if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
            if (entry.first.find('/', prefix.size()) == std::string::npos) {
                children.push_back(entry.first);
            }
        }
        return true;
    }
    bool remove_group(const std::string &path, std::string &) override {
        removed.insert(path);
        for (auto iter = groups.begin(); iter != groups.end();) {
            if (iter->first == path || (iter->first.size() > path.size() &&
                                        iter->first.compare(0, path.size(), path) == 0 &&
                                        iter->first[path.size()] == '/')) {
                iter = groups.erase(iter);
            } else {
                ++iter;
            }
        }
        return true;
    }
    int signal_pid(pid_t pid, int signo) override {
        signals.emplace_back(pid, signo);
        if (signo == SIGKILL) {
            for (auto &entry : groups) {
                auto &members = entry.second;
                members.erase(std::remove(members.begin(), members.end(), pid), members.end());
            }
        }
        return 0;
    }
};

int main() {
    auto fs = std::make_shared<fake_cgroup_fs>();
    pm_tiny::cgroup_v2_backend backend("/fake", fs);
    pm_tiny::process_tree_handle handle;
    std::string reason;
    if (!backend.attach(42, handle, reason) || !handle.active) return 1;
    fs->groups[handle.cgroup_path + "/child"] = {43, 44};
    if (!backend.contains(handle, 42) || !backend.contains(handle, 44) || backend.contains(handle, 99)) return 21;
    if (backend.empty(handle)) return 2;
    if (backend.signal(handle, SIGTERM) != 0 || fs->signals.size() != 3) return 3;
    if (backend.signal(handle, SIGKILL) != 0 || !backend.empty(handle)) return 4;
    const std::string app_path = handle.cgroup_path;
    backend.cleanup(handle);
    if (handle.active || !fs->removed.count(app_path) ||
        fs->groups.count(app_path + "/child")) return 5;

    pm_tiny::process_tree_handle failed;
    fs->fail_attach = true;
    if (backend.attach(55, failed, reason) || failed.active) return 6;
    if (!fs->removed.count("/fake/app-55")) return 7;

    fs->fail_attach = false;
    pm_tiny::process_tree_handle unreadable;
    if (!backend.attach(66, unreadable, reason)) return 8;
    fs->fail_collect = true;
    if (backend.contains(unreadable, 66)) return 22;
    if (backend.empty(unreadable)) return 9;
    errno = 0;
    if (backend.signal(unreadable, SIGTERM) != -1 || errno != EIO) return 10;
    const std::string unreadable_path = unreadable.cgroup_path;
    backend.cleanup(unreadable);
    if (fs->removed.count(unreadable_path)) return 11;

    fs->fail_collect = false;
    fs->fail_create = true;
    pm_tiny::process_tree_handle uncreated;
    if (backend.attach(77, uncreated, reason) || uncreated.active) return 12;

    auto controller_fs = std::make_shared<fake_cgroup_fs>();
    const std::string stale_path = "/fake/pm_tiny-unit/app-88";
    controller_fs->groups[stale_path] = {88};
    pm_tiny::process_tree_controller controller(controller_fs);
    if (!controller.initialize(pm_tiny::process_tree_mode::cgroup, "/fake", "unit", reason)) return 13;
    if (controller.effective_mode() != pm_tiny::process_tree_mode::cgroup) return 14;
    if (!controller_fs->removed.count(stale_path)) return 15;

    auto unavailable_fs = std::make_shared<fake_cgroup_fs>();
    unavailable_fs->available = false;
    pm_tiny::process_tree_controller auto_controller(unavailable_fs);
    if (!auto_controller.initialize(pm_tiny::process_tree_mode::auto_detect,
                                    "/fake", "unit", reason)) return 16;
    if (auto_controller.effective_mode() != pm_tiny::process_tree_mode::process_group) return 17;
    if (!auto_controller.degraded() ||
        auto_controller.degradation_reason() != "cgroup v2 cgroup.procs is unavailable") return 18;
    pm_tiny::process_tree_controller strict_controller(unavailable_fs);
    if (strict_controller.initialize(pm_tiny::process_tree_mode::cgroup,
                                     "/fake", "unit", reason)) return 19;
    if (strict_controller.degraded()) return 20;
    return 0;
}
