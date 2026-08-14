#include "process_list.h"
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

} // namespace

int main() {
    pm_tiny::process_list_entry input;
    input.pid = 1234;
    input.name = "中文服务";
    input.cwd = "/srv/app";
    input.executable = "./server";
    input.args = {"--flag", "value with space", ""};
    input.restart_count = 7;
    input.state = 1;
    input.has_uptime = true;
    input.uptime_ms = 65432;
    input.has_rss = true;
    input.rss_kib = 4096;
    input.daemon = true;
    input.pty = pm_tiny::pty_mode_t::enabled;
    input.depends_on = {"database", "cache"};
    input.restart_pending = true;
    input.restart_delay_remaining_ms = 2500;
    input.restart_attempts_in_window = 4;
    input.restart_suppressed = true;
    input.restart_suppression_reason = pm_tiny::restart_attempt_limit_reason;
    input.generation = 42;
    input.ready = true;
    input.heartbeat_enabled = true;
    input.has_last_exit = true;
    input.last_exit_reason = "signaled";
    input.last_exit_code = 9;
    input.process_tree_backend = "cgroup";
    input.process_tree_degraded = true;
    input.process_tree_degradation_reason = "test fallback";
    input.config_source = "runtime";
    input.log_degraded = true;
    input.log_dropped_bytes = 123;
    input.log_last_error = "disk full";
    input.log_retry_remaining_ms = 2000;
    input.log_paths = {"/logs/app_stdout.log", "/logs/app_stderr.log"};

    pm_tiny::frame_t frame;
    pm_tiny::append_process_list(frame, {input});
    pm_tiny::iframe_stream stream(frame);
    const auto decoded = pm_tiny::read_process_list(stream);
    expect(decoded.size() == 1, "entry count should round-trip");
    const auto &actual = decoded.front();
    expect(actual.pid == input.pid && actual.name == input.name, "identity should round-trip");
    expect(actual.cwd == input.cwd && actual.executable == input.executable && actual.args == input.args,
           "argv fields should round-trip");
    expect(actual.restart_count == input.restart_count && actual.state == input.state,
           "runtime counters should round-trip");
    expect(actual.has_uptime && actual.uptime_ms == input.uptime_ms, "uptime should round-trip");
    expect(actual.has_rss && actual.rss_kib == input.rss_kib, "RSS should round-trip");
    expect(actual.daemon && actual.pty == input.pty, "flags should round-trip");
    expect(actual.depends_on == input.depends_on, "dependencies should round-trip");
    expect(actual.restart_pending == input.restart_pending &&
           actual.restart_delay_remaining_ms == input.restart_delay_remaining_ms &&
           actual.restart_attempts_in_window == input.restart_attempts_in_window &&
           actual.restart_suppressed == input.restart_suppressed &&
           actual.restart_suppression_reason == input.restart_suppression_reason,
           "restart runtime state should round-trip");
    expect(actual.generation == input.generation && actual.ready && actual.heartbeat_enabled,
           "health runtime state should round-trip");
    expect(actual.has_last_exit && actual.last_exit_reason == input.last_exit_reason &&
           actual.last_exit_code == input.last_exit_code,
           "last exit state should round-trip");
    expect(actual.process_tree_backend == input.process_tree_backend && actual.process_tree_degraded &&
           actual.process_tree_degradation_reason == input.process_tree_degradation_reason &&
           actual.config_source == input.config_source,
           "diagnostic state should round-trip");
    expect(actual.log_degraded && actual.log_dropped_bytes == 123 &&
           actual.log_last_error == "disk full" && actual.log_retry_remaining_ms == 2000 &&
           actual.log_paths == input.log_paths, "log health should round-trip");

    pm_tiny::frame_t invalid;
    pm_tiny::fappend_value<std::int32_t>(invalid, pm_tiny::process_list_schema_version + 1);
    pm_tiny::fappend_value<std::int32_t>(invalid, 0);
    try {
        pm_tiny::iframe_stream invalid_stream(invalid);
        (void)pm_tiny::read_process_list(invalid_stream);
        expect(false, "unknown schema should fail");
    } catch (const pm_tiny::protocol_error &) {
    }
    return 0;
}
