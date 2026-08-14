#include "platform/linux/process_tree_controller.h"

#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    pm_tiny::process_tree_mode mode;
    if (!pm_tiny::parse_process_tree_mode("auto", mode) ||
        mode != pm_tiny::process_tree_mode::auto_detect) return 1;
    if (!pm_tiny::parse_process_tree_mode("cgroup", mode) ||
        mode != pm_tiny::process_tree_mode::cgroup) return 2;
    if (!pm_tiny::parse_process_tree_mode("process_group", mode) ||
        mode != pm_tiny::process_tree_mode::process_group) return 3;
    if (pm_tiny::parse_process_tree_mode("invalid", mode)) return 4;

    pm_tiny::process_tree_controller controller;
    std::string reason;
    if (!controller.initialize(pm_tiny::process_tree_mode::process_group, "", "test", reason)) return 5;
    if (controller.effective_mode() != pm_tiny::process_tree_mode::process_group) return 6;

    pid_t pid = fork();
    if (pid < 0) return 7;
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(2);
        for (;;) pause();
    }
    if (setpgid(pid, pid) != 0 && errno != EACCES) return 8;
    pm_tiny::process_tree_handle handle;
    if (!controller.attach(pid, handle, reason)) return 9;
    if (handle.mode != pm_tiny::process_tree_mode::process_group || controller.empty(handle)) return 10;
    if (controller.signal(handle, SIGTERM) != 0) return 11;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return 12;
    if (!controller.empty(handle)) return 13;
    controller.cleanup(handle);
    if (handle.active) return 14;
    return 0;
}
