#include "cgroup_fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pm_tiny {
namespace {

bool is_cgroup2_path(const std::string &path) {
    std::ifstream mounts("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mounts, line)) {
        const std::string marker = " - cgroup2 ";
        auto separator = line.find(marker);
        if (separator == std::string::npos) continue;
        std::istringstream fields(line.substr(0, separator));
        std::string id, parent, major_minor, root, mountpoint, options;
        fields >> id >> parent >> major_minor >> root >> mountpoint >> options;
        if (mountpoint == path ||
            (path.size() > mountpoint.size() &&
             path.compare(0, mountpoint.size(), mountpoint) == 0 &&
             path[mountpoint.size()] == '/')) return true;
    }
    return false;
}

bool collect_recursive(const std::string &group, std::vector<pid_t> &pids, std::string &reason) {
    std::ifstream procs(group + "/cgroup.procs");
    if (!procs.is_open()) {
        reason = strerror(errno);
        return false;
    }
    int pid;
    while (procs >> pid) pids.push_back(static_cast<pid_t>(pid));
    DIR *dir = ::opendir(group.c_str());
    if (!dir) {
        reason = strerror(errno);
        return false;
    }
    bool ok = true;
    while (auto *entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        std::string child = group + "/" + entry->d_name;
        struct stat st{};
        if (::stat(child.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            reason = strerror(errno);
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode) && !collect_recursive(child, pids, reason)) {
            ok = false;
            break;
        }
    }
    ::closedir(dir);
    return ok;
}

bool remove_recursive(const std::string &group, std::string &reason) {
    DIR *dir = ::opendir(group.c_str());
    if (!dir) {
        if (errno == ENOENT) return true;
        reason = strerror(errno);
        return false;
    }
    bool ok = true;
    while (auto *entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        std::string child = group + "/" + entry->d_name;
        struct stat st{};
        if (::stat(child.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            reason = strerror(errno);
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode) && !remove_recursive(child, reason)) {
            ok = false;
            break;
        }
    }
    ::closedir(dir);
    if (!ok) return false;
    if (::rmdir(group.c_str()) == 0 || errno == ENOENT) return true;
    reason = strerror(errno);
    return false;
}

} // namespace

bool posix_cgroup_fs::is_v2_available(const std::string &root) const {
    struct stat st{};
    return is_cgroup2_path(root) &&
           ::stat((root + "/cgroup.procs").c_str(), &st) == 0 &&
           ::stat((root + "/cgroup.controllers").c_str(), &st) == 0;
}

bool posix_cgroup_fs::create_group(const std::string &path, std::string &reason) {
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    if (errno == 0) errno = EIO;
    reason = strerror(errno);
    return false;
}

bool posix_cgroup_fs::attach_pid(const std::string &group, pid_t pid, std::string &reason) {
    int fd = ::open((group + "/cgroup.procs").c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        reason = strerror(errno);
        return false;
    }
    std::string value = std::to_string(static_cast<int>(pid));
    ssize_t written = ::write(fd, value.data(), value.size());
    int saved = errno;
    ::close(fd);
    errno = saved;
    if (written == static_cast<ssize_t>(value.size())) return true;
    if (errno == 0) errno = EIO;
    reason = strerror(errno);
    return false;
}

bool posix_cgroup_fs::collect_pids(const std::string &group, std::vector<pid_t> &pids,
                                   std::string &reason) const {
    return collect_recursive(group, pids, reason);
}

bool posix_cgroup_fs::list_child_groups(const std::string &group,
                                        std::vector<std::string> &children,
                                        std::string &reason) const {
    DIR *dir = ::opendir(group.c_str());
    if (!dir) {
        reason = strerror(errno);
        return false;
    }
    while (auto *entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        std::string child = group + "/" + entry->d_name;
        struct stat st{};
        if (::stat(child.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            reason = strerror(errno);
            ::closedir(dir);
            return false;
        }
        if (S_ISDIR(st.st_mode)) children.push_back(child);
    }
    ::closedir(dir);
    return true;
}

bool posix_cgroup_fs::remove_group(const std::string &path, std::string &reason) {
    return remove_recursive(path, reason);
}

int posix_cgroup_fs::signal_pid(pid_t pid, int signo) {
    return ::kill(pid, signo);
}

} // namespace pm_tiny
