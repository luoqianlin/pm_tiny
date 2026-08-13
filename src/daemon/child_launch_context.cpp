#include "child_launch_context.h"

#include <cerrno>
#include <unistd.h>

namespace pm_tiny {

static void close_fd(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

int child_launch_context::prepare(bool pty_mode) {
    use_pty = pty_mode;
    if (!use_pty) {
        if (pipe(stdout_pipe) == -1) return -1;
        if (pipe(stderr_pipe) == -1) return -1;
    } else if (create_pty(&pty) != 0) {
        return -1;
    }
    if (pipe(gate) == -1) return -1;
    if (pipe(exec_status) == -1 || set_cloexec(exec_status[1]) == -1) return -1;
    return 0;
}

void child_launch_context::close_parent_ends() {
    close_fd(gate[0]);
    close_fd(exec_status[1]);
    if (!use_pty) {
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[1]);
    }
}

void child_launch_context::close_child_ends() {
    close_fd(gate[1]);
    close_fd(exec_status[0]);
    if (!use_pty) {
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
    } else {
        close_fd(pty.master_fd);
    }
}

void child_launch_context::release_parent_streams(int &stdout_fd, int &stderr_fd) {
    stdout_fd = use_pty ? pty.master_fd : stdout_pipe[0];
    stderr_fd = use_pty ? -1 : stderr_pipe[0];
    if (use_pty) pty.master_fd = -1;
    else {
        stdout_pipe[0] = -1;
        stderr_pipe[0] = -1;
    }
}

void child_launch_context::close_all() {
    for (int *fd : {stdout_pipe, stderr_pipe, gate, exec_status}) {
        close_fd(fd[0]);
        close_fd(fd[1]);
    }
    close_fd(pty.master_fd);
}

} // namespace pm_tiny
