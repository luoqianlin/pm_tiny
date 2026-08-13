#include "termination_job.h"
#include <algorithm>

namespace pm_tiny {
termination_action termination_job::request(uint64_t generation, int64_t now_ms, int timeout_sec) {
    if (phase_ != termination_phase::none && generation_ == generation) return termination_action::none;
    generation_ = generation;
    phase_ = termination_phase::term_requested;
    started_ms_ = now_ms;
    timeout_ms_ = std::max<int64_t>(0, static_cast<int64_t>(timeout_sec) * 1000);
    return termination_action::send_term;
}
termination_action termination_job::force(uint64_t generation) {
    if (generation_ != generation || phase_ == termination_phase::completed) return termination_action::none;
    phase_ = termination_phase::force_kill_requested;
    return termination_action::send_kill;
}
termination_action termination_job::mark_tree_draining(uint64_t generation, int64_t now_ms, int timeout_sec) {
    if (phase_ == termination_phase::none || generation_ != generation) {
        generation_ = generation;
        started_ms_ = now_ms;
        timeout_ms_ = std::max<int64_t>(0, static_cast<int64_t>(timeout_sec) * 1000);
    } else if (phase_ == termination_phase::completed ||
               phase_ == termination_phase::force_kill_requested) {
        return termination_action::none;
    }
    phase_ = termination_phase::tree_draining;
    return termination_action::send_term;
}
termination_action termination_job::poll(uint64_t generation, int64_t now_ms, bool tree_empty) {
    if (generation_ != generation) return termination_action::none;
    if (tree_empty) {
        if (phase_ == termination_phase::none || phase_ == termination_phase::completed) return termination_action::none;
        phase_ = termination_phase::completed;
        return termination_action::complete;
    }
    if ((phase_ == termination_phase::term_requested || phase_ == termination_phase::tree_draining) &&
        now_ms - started_ms_ >= timeout_ms_) {
        phase_ = termination_phase::force_kill_requested;
        return termination_action::send_kill;
    }
    return termination_action::none;
}
} // namespace pm_tiny
