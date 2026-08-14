#ifndef PM_TINY_TERMINATION_JOB_H
#define PM_TINY_TERMINATION_JOB_H

#include <cstdint>

namespace pm_tiny {

enum class termination_phase { none, term_requested, force_kill_requested, tree_draining, completed };
enum class termination_action { none, send_term, send_kill, complete };

class termination_job {
public:
    termination_action request(std::uint64_t generation, std::int64_t now_ms, int timeout_sec);
    termination_action force(std::uint64_t generation);
    termination_action mark_tree_draining(std::uint64_t generation, std::int64_t now_ms, int timeout_sec);
    termination_action poll(std::uint64_t generation, std::int64_t now_ms, bool tree_empty);

    termination_phase phase() const { return phase_; }
    std::int64_t deadline_ms() const { return started_ms_ + timeout_ms_; }
    std::uint64_t generation() const { return generation_; }

private:
    std::uint64_t generation_ = 0;
    termination_phase phase_ = termination_phase::none;
    std::int64_t started_ms_ = 0;
    std::int64_t timeout_ms_ = 0;
};

} // namespace pm_tiny

#endif
