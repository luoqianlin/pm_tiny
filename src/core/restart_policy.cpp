#include "restart_policy.h"

#include <algorithm>
#include <limits>

namespace pm_tiny {

void restart_policy_state::reset() {
    attempts_ms.clear();
    consecutive_failures = 0;
    suppressed = false;
}

restart_decision plan_automatic_restart(const restart_policy_config &config,
                                        restart_policy_state &state,
                                        std::int64_t now_ms,
                                        std::int64_t runtime_ms) {
    if (config.reset_after_ms > 0 && runtime_ms >= config.reset_after_ms) state.reset();

    const auto window_ms = std::max(0, config.window_ms);
    while (!state.attempts_ms.empty() && now_ms - state.attempts_ms.front() >= window_ms) {
        state.attempts_ms.pop_front();
    }

    restart_decision decision;
    decision.attempts_in_window = static_cast<int>(state.attempts_ms.size());
    if (config.max_attempts > 0 && decision.attempts_in_window >= config.max_attempts) {
        state.suppressed = true;
        decision.suppressed = true;
        return decision;
    }

    const auto base_delay = std::max(0, config.delay_ms);
    const auto maximum_delay = std::max(base_delay, config.max_delay_ms);
    std::int64_t delay = base_delay;
    for (int i = 0; i < state.consecutive_failures && delay < maximum_delay; ++i) {
        delay = std::min<std::int64_t>(maximum_delay, delay * 2);
    }
    decision.restart = true;
    decision.delay_ms = static_cast<int>(std::min<std::int64_t>(delay, std::numeric_limits<int>::max()));
    state.attempts_ms.push_back(now_ms);
    ++state.consecutive_failures;
    state.suppressed = false;
    decision.attempts_in_window = static_cast<int>(state.attempts_ms.size());
    return decision;
}

} // namespace pm_tiny
