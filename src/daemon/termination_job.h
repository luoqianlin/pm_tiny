#ifndef PM_TINY_TERMINATION_JOB_H
#define PM_TINY_TERMINATION_JOB_H

#include <cstdint>

namespace pm_tiny {
enum class termination_phase { none, term_requested, force_kill_requested, tree_draining, completed };
enum class termination_action { none, send_term, send_kill, complete };

class termination_job {
public:
    termination_action request(uint64_t generation, int64_t now_ms, int timeout_sec);
    termination_action force(uint64_t generation);
    termination_action mark_tree_draining(uint64_t generation, int64_t now_ms, int timeout_sec);
    termination_action poll(uint64_t generation, int64_t now_ms, bool tree_empty);
    termination_phase phase() const { return phase_; }
private:
    uint64_t generation_ = 0;
    termination_phase phase_ = termination_phase::none;
    int64_t started_ms_ = 0;
    int64_t timeout_ms_ = 0;
};
} // namespace pm_tiny

#endif
