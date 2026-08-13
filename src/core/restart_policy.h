#ifndef PM_TINY_RESTART_POLICY_H
#define PM_TINY_RESTART_POLICY_H

#include <cstdint>
#include <deque>

namespace pm_tiny {

constexpr const char *restart_attempt_limit_reason = "restart attempt limit reached";

struct restart_policy_config {
    int delay_ms = 1000;
    int max_delay_ms = 30000;
    int window_ms = 60000;
    int max_attempts = 10;
    int reset_after_ms = 60000;
};

struct restart_policy_state {
    std::deque<std::int64_t> attempts_ms;
    int consecutive_failures = 0;
    bool suppressed = false;

    void reset();
};

struct restart_decision {
    bool restart = false;
    bool suppressed = false;
    int delay_ms = 0;
    int attempts_in_window = 0;
};

restart_decision plan_automatic_restart(const restart_policy_config &config,
                                        restart_policy_state &state,
                                        std::int64_t now_ms,
                                        std::int64_t runtime_ms);

} // namespace pm_tiny

#endif
