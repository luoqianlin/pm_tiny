#include "child_launch_context.h"

#include <fcntl.h>
#include <unistd.h>

static int test_non_pty_does_not_own_stdin() {
    const int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0) return 1;
    close(STDIN_FILENO);
    if (open("/dev/null", O_RDONLY) != STDIN_FILENO) return 1;

    int stdout_fd = -1;
    int stderr_fd = -1;
    {
        pm_tiny::child_launch_context launch;
        if (launch.prepare(false) != 0) return 1;
        launch.close_parent_ends();
        launch.release_parent_streams(stdout_fd, stderr_fd);
    }

    const bool stdin_open = fcntl(STDIN_FILENO, F_GETFD) != -1;
    const bool streams_open = fcntl(stdout_fd, F_GETFD) != -1 &&
                              fcntl(stderr_fd, F_GETFD) != -1;
    close(stdout_fd);
    close(stderr_fd);
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    return stdin_open && streams_open ? 0 : 1;
}

int main() {
    return test_non_pty_does_not_own_stdin();
}
