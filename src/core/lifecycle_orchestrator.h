#ifndef PM_TINY_LIFECYCLE_ORCHESTRATOR_H
#define PM_TINY_LIFECYCLE_ORCHESTRATOR_H

#include "pm_tiny_enum.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pm_tiny {

enum class lifecycle_timeout_kind {
    startup,
    heartbeat
};

enum class lifecycle_timeout_action {
    accept_ready,
    ignore,
    restart,
    reboot
};

struct lifecycle_process_observation {
    std::size_t process_index = 0;
    std::uint64_t generation = 0;
    bool has_process = false;
    bool starting = false;
    bool online = false;
    bool terminating = false;
    bool tree_draining = false;
    bool restart_pending = false;
    std::int64_t restart_due_ms = 0;
    std::int64_t launch_time_ms = 0;
    std::int64_t last_tick_ms = 0;
    std::int64_t heartbeat_action_due_ms = 0;
    std::int64_t termination_due_ms = 0;
    int start_timeout_s = 0;
    int heartbeat_timeout_s = -1;
    failure_action_t failure_action = failure_action_t::SKIP;
};

struct lifecycle_timeout_event {
    std::size_t process_index = 0;
    std::uint64_t generation = 0;
    lifecycle_timeout_kind kind = lifecycle_timeout_kind::startup;
    lifecycle_timeout_action action = lifecycle_timeout_action::ignore;
};

struct lifecycle_tick_plan {
    std::vector<lifecycle_timeout_event> timeouts;
    std::vector<std::size_t> due_restarts;
    int next_wait_ms = 1000;
};

lifecycle_timeout_action decide_lifecycle_timeout(lifecycle_timeout_kind kind,
                                                  failure_action_t action);

lifecycle_tick_plan plan_lifecycle_tick(
    std::int64_t now_ms,
    const std::vector<lifecycle_process_observation> &processes,
    bool busy,
    int default_wait_ms = 1000,
    int draining_wait_ms = 25);

} // namespace pm_tiny

#endif
