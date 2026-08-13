#include "process_reaper.h"

#include "pm_sys.h"

#include <errno.h>
#include <sstream>
#include <sys/wait.h>

namespace pm_tiny {

std::vector<reaped_child> process_reaper::reap_all() const {
    std::vector<reaped_child> result;
    for (;;) {
        int status = 0;
        pid_t pid = safe_waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            result.push_back({pid, status});
            continue;
        }
        if (pid == -1 && errno != ECHILD) {
            // The caller will continue operating; the signal-safe reaper has
            // no logger dependency and leaves diagnostics to its caller.
        }
        break;
    }
    return result;
}

std::string describe_reaped_descendant(const reaped_child &child) {
    std::ostringstream output;
    output << "reaped descendant pid=" << child.pid;
    if (WIFEXITED(child.status)) {
        output << " exit_code=" << WEXITSTATUS(child.status);
    } else if (WIFSIGNALED(child.status)) {
        output << " signal=" << WTERMSIG(child.status);
    } else {
        output << " status=" << child.status;
    }
    return output.str();
}

} // namespace pm_tiny
