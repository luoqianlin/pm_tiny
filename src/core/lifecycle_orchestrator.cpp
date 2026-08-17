#include "lifecycle_orchestrator.h"

#include <algorithm>
#include <limits>

namespace pm_tiny {

lifecycle_timeout_action decide_lifecycle_timeout(lifecycle_timeout_kind kind,
                                                  failure_action_t action) {
    switch (action) {
        case failure_action_t::SKIP:
            return kind == lifecycle_timeout_kind::startup
                ? lifecycle_timeout_action::accept_ready
                : lifecycle_timeout_action::ignore;
        case failure_action_t::RESTART:
            return lifecycle_timeout_action::restart;
        case failure_action_t::REBOOT:
            return lifecycle_timeout_action::reboot;
    }
    return lifecycle_timeout_action::ignore;
}

lifecycle_tick_plan plan_lifecycle_tick(
    std::int64_t now_ms,
    const std::vector<lifecycle_process_observation> &processes,
    bool busy,
    int default_wait_ms,
    int draining_wait_ms) {
    lifecycle_tick_plan result;
    const auto default_wait = std::max(1, default_wait_ms);
    std::int64_t next_due_ms = now_ms + default_wait;
    bool draining = false;
    const auto consider = [&](std::int64_t due_ms) {
        next_due_ms = std::min(next_due_ms, due_ms);
    };

    for (const auto &process : processes) {
        if (!process.has_process) {
            if (process.restart_pending) {
                if (process.restart_due_ms <= now_ms)
                    result.due_restarts.push_back(process.process_index);
                else
                    consider(process.restart_due_ms);
            }
            continue;
        }
        if (process.terminating) {
            draining = draining || process.tree_draining;
            if (process.termination_due_ms > 0) consider(process.termination_due_ms);
            continue;
        }

        std::int64_t timeout_due_ms = std::numeric_limits<std::int64_t>::max();
        lifecycle_timeout_kind kind = lifecycle_timeout_kind::startup;
        bool timeout_enabled = false;
        if (process.starting && process.start_timeout_s >= 0) {
            timeout_due_ms = process.launch_time_ms +
                static_cast<std::int64_t>(process.start_timeout_s) * 1000;
            timeout_enabled = true;
        } else if (process.online && process.heartbeat_timeout_s > 0) {
            timeout_due_ms = process.last_tick_ms +
                static_cast<std::int64_t>(process.heartbeat_timeout_s) * 1000;
            timeout_due_ms = std::max(timeout_due_ms, process.heartbeat_action_due_ms);
            kind = lifecycle_timeout_kind::heartbeat;
            timeout_enabled = true;
        }
        if (!timeout_enabled) continue;
        if (timeout_due_ms <= now_ms) {
            result.timeouts.push_back({process.process_index, process.generation, kind,
                                       decide_lifecycle_timeout(kind, process.failure_action)});
        } else {
            consider(timeout_due_ms);
        }
    }

    if (!result.timeouts.empty() || !result.due_restarts.empty()) {
        result.next_wait_ms = 1;
    } else if (busy) {
        result.next_wait_ms = 10;
    } else if (draining) {
        result.next_wait_ms = std::max(1, draining_wait_ms);
    } else if (next_due_ms <= now_ms) {
        result.next_wait_ms = 1;
    } else {
        result.next_wait_ms = static_cast<int>(std::min<std::int64_t>(
            default_wait, next_due_ms - now_ms));
    }
    return result;
}

} // namespace pm_tiny
