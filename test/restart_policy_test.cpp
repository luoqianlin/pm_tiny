#include "restart_policy.h"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}
}

int main() {
    pm_tiny::restart_policy_config config;
    config.delay_ms = 1000;
    config.max_delay_ms = 4000;
    config.window_ms = 10000;
    config.max_attempts = 3;
    config.reset_after_ms = 5000;
    pm_tiny::restart_policy_state state;

    auto first = pm_tiny::plan_automatic_restart(config, state, 1000, 100);
    auto second = pm_tiny::plan_automatic_restart(config, state, 2000, 100);
    auto third = pm_tiny::plan_automatic_restart(config, state, 3000, 100);
    expect(first.restart && first.delay_ms == 1000, "first restart should use base delay");
    expect(second.restart && second.delay_ms == 2000, "second restart should back off");
    expect(third.restart && third.delay_ms == 4000, "delay should reach cap");

    auto suppressed = pm_tiny::plan_automatic_restart(config, state, 4000, 100);
    expect(!suppressed.restart && suppressed.suppressed, "window limit should suppress restart");

    auto expired = pm_tiny::plan_automatic_restart(config, state, 12000, 100);
    expect(expired.restart && !expired.suppressed, "expired attempts should leave the window");

    auto stable = pm_tiny::plan_automatic_restart(config, state, 13000, 5000);
    expect(stable.restart && stable.delay_ms == 1000 && stable.attempts_in_window == 1,
           "stable runtime should reset backoff and window state");

    state.reset();
    config.max_attempts = 0;
    for (int i = 0; i < 20; ++i) {
        expect(pm_tiny::plan_automatic_restart(config, state, i * 100, 10).restart,
               "zero max attempts should disable suppression");
    }
    return 0;
}
