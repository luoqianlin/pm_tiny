#include "lifecycle_orchestrator.h"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char *message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

void test_timeout_decisions() {
    using namespace pm_tiny;
    expect(decide_lifecycle_timeout(lifecycle_timeout_kind::startup, failure_action_t::SKIP) ==
               lifecycle_timeout_action::accept_ready,
           "startup skip should accept ready");
    expect(decide_lifecycle_timeout(lifecycle_timeout_kind::heartbeat, failure_action_t::SKIP) ==
               lifecycle_timeout_action::ignore,
           "heartbeat skip should preserve the process");
    expect(decide_lifecycle_timeout(lifecycle_timeout_kind::startup, failure_action_t::RESTART) ==
               lifecycle_timeout_action::restart,
           "startup restart should restart");
    expect(decide_lifecycle_timeout(lifecycle_timeout_kind::heartbeat, failure_action_t::REBOOT) ==
               lifecycle_timeout_action::reboot,
           "heartbeat reboot should reboot");
    expect(decide_lifecycle_timeout(lifecycle_timeout_kind::heartbeat,
                                    static_cast<failure_action_t>(99)) ==
               lifecycle_timeout_action::ignore,
           "unknown failure action should fail closed");
}

void test_tick_planning() {
    using namespace pm_tiny;
    lifecycle_process_observation startup;
    startup.process_index = 3;
    startup.generation = 8;
    startup.has_process = true;
    startup.starting = true;
    startup.launch_time_ms = 1000;
    startup.start_timeout_s = 2;
    startup.failure_action = failure_action_t::RESTART;
    auto plan = plan_lifecycle_tick(3000, {startup}, false);
    expect(plan.timeouts.size() == 1 && plan.timeouts[0].process_index == 3 &&
               plan.timeouts[0].generation == 8 &&
               plan.timeouts[0].action == lifecycle_timeout_action::restart,
           "due startup timeout should retain identity and action");

    lifecycle_process_observation heartbeat;
    heartbeat.process_index = 4;
    heartbeat.generation = 9;
    heartbeat.has_process = true;
    heartbeat.online = true;
    heartbeat.last_tick_ms = 1000;
    heartbeat.heartbeat_timeout_s = 1;
    heartbeat.heartbeat_action_due_ms = 4500;
    plan = plan_lifecycle_tick(4000, {heartbeat}, false);
    expect(plan.timeouts.empty() && plan.next_wait_ms == 500,
           "heartbeat skip throttle should prevent a busy loop");

    lifecycle_process_observation restart;
    restart.process_index = 5;
    restart.restart_pending = true;
    restart.restart_due_ms = 5000;
    plan = plan_lifecycle_tick(5000, {restart}, false);
    expect(plan.due_restarts.size() == 1 && plan.due_restarts[0] == 5,
           "due restart should be scheduled");

    restart.restart_due_ms = 5500;
    plan = plan_lifecycle_tick(5000, {restart}, false);
    expect(plan.due_restarts.empty() && plan.next_wait_ms == 500,
           "future restart should determine the next wakeup");
    restart.restart_pending = false;
    plan = plan_lifecycle_tick(5000, {restart}, false);
    expect(plan.due_restarts.empty() && plan.next_wait_ms == 1000,
           "idle process should use the default wakeup");

    lifecycle_process_observation disabled;
    disabled.has_process = true;
    disabled.starting = true;
    disabled.start_timeout_s = -1;
    plan = plan_lifecycle_tick(5000, {disabled}, false);
    expect(plan.timeouts.empty(), "negative start timeout should wait for ready indefinitely");
    disabled.starting = false;
    disabled.online = true;
    disabled.heartbeat_timeout_s = 0;
    plan = plan_lifecycle_tick(5000, {disabled}, false);
    expect(plan.timeouts.empty(), "disabled heartbeat should not emit an event");
    disabled.online = false;
    plan = plan_lifecycle_tick(5000, {disabled}, false);
    expect(plan.timeouts.empty(), "inactive lifecycle state should not emit an event");

    heartbeat.heartbeat_action_due_ms = 0;
    heartbeat.failure_action = failure_action_t::SKIP;
    plan = plan_lifecycle_tick(2500, {heartbeat}, false);
    expect(plan.timeouts.size() == 1 &&
               plan.timeouts[0].kind == lifecycle_timeout_kind::heartbeat &&
               plan.timeouts[0].action == lifecycle_timeout_action::ignore,
           "due heartbeat should use heartbeat failure policy");

    lifecycle_process_observation terminating;
    terminating.has_process = true;
    terminating.terminating = true;
    terminating.tree_draining = true;
    plan = plan_lifecycle_tick(1000, {terminating}, false, 1000, 25);
    expect(plan.next_wait_ms == 25, "tree draining should use the short poll interval");
    plan = plan_lifecycle_tick(1000, {terminating}, false, 1000, 0);
    expect(plan.next_wait_ms == 1, "tree draining interval should stay positive");
    terminating.tree_draining = false;
    terminating.termination_due_ms = 900;
    plan = plan_lifecycle_tick(1000, {terminating}, false);
    expect(plan.next_wait_ms == 1, "past termination deadline should wake immediately");
    terminating.termination_due_ms = 1500;
    plan = plan_lifecycle_tick(1000, {terminating}, false);
    expect(plan.next_wait_ms == 500, "future termination deadline should determine wakeup");
    plan = plan_lifecycle_tick(1000, {}, true);
    expect(plan.next_wait_ms == 10, "busy persistence should use the busy interval");
    plan = plan_lifecycle_tick(1000, {}, false, 0);
    expect(plan.next_wait_ms == 1, "default interval should stay positive");
}

} // namespace

int main() {
    test_timeout_decisions();
    test_tick_planning();
    return 0;
}
