#include "process_reaper.h"

#include <cassert>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <string>

using pm_tiny::process_reaper;

static void expect_no_children() {
    process_reaper reaper;
    const auto children = reaper.reap_all();
    assert(children.empty());
}

static int expect_reaps_all_children() {
    constexpr int count = 3;
    for (int i = 0; i < count; ++i) {
        const pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) _exit(10 + i);
    }

    std::vector<pm_tiny::reaped_child> children;
    for (int retry = 0; retry < 100 && children.size() < count; ++retry) {
        const auto batch = process_reaper{}.reap_all();
        children.insert(children.end(), batch.begin(), batch.end());
        if (children.size() < count) usleep(1000);
    }
    assert(children.size() == count);
    for (const auto &child : children) {
        if (child.pid <= 0 || !WIFEXITED(child.status) || WEXITSTATUS(child.status) < 10) {
            return 1;
        }
        const std::string description = pm_tiny::describe_reaped_descendant(child);
        if (description.find("reaped descendant pid=") == std::string::npos ||
            description.find("exit_code=") == std::string::npos ||
            description.find("nan") != std::string::npos ||
            description.find("Unkown") != std::string::npos) return 1;
    }

    errno = 0;
    int status = 0;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) return 1;
    return 0;
}

int main() {
    expect_no_children();
    return expect_reaps_all_children();
}
