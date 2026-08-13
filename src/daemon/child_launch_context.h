#ifndef PM_TINY_CHILD_LAUNCH_CONTEXT_H
#define PM_TINY_CHILD_LAUNCH_CONTEXT_H

#include "pm_sys.h"

namespace pm_tiny {

// Owns the descriptors used during fork/exec.  The owner explicitly releases
// descriptors that are transferred to prog_info_t after a successful spawn.
struct child_launch_context {
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    int gate[2]{-1, -1};
    int exec_status[2]{-1, -1};
    pty_info pty{-1, {}};
    bool use_pty = false;

    int prepare(bool pty_mode);
    void close_parent_ends();
    void close_child_ends();
    void release_parent_streams(int &stdout_fd, int &stderr_fd);
    void close_all();
    ~child_launch_context() { close_all(); }
};

} // namespace pm_tiny

#endif
