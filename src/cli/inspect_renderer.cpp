#include "inspect_renderer.h"

#include "pm_tiny.h"

#include <fort.hpp>

#include <sstream>

namespace pm_tiny {
namespace cli {
namespace {

std::string join(const std::vector<std::string> &values, const char *separator) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty()) result += separator;
        result += value;
    }
    return result;
}

const char *bool_text(bool value) { return value ? "Y" : "N"; }

} // namespace

std::string render_inspect_snapshot(const inspect_snapshot &snapshot) {
    const auto &config = snapshot.config;
    const auto &runtime = snapshot.runtime;
    fort::utf8_table table;
    table.set_border_style(FT_BASIC_STYLE);
    table << "name" << config.name << fort::endr;
    table << "state" << pm_state_to_str(runtime.state) << fort::endr;
    table << "pid" << (runtime.pid >= 0 ? std::to_string(runtime.pid) : "-") << fort::endr;
    table << "generation" << runtime.generation << fort::endr;
    table << "cwd" << config.cwd << fort::endr;
    table << "executable" << config.executable << fort::endr;
    table << "args" << join(config.args, " | ") << fort::endr;
    table << "user" << (config.run_as.empty() ? "-" : config.run_as) << fort::endr;
    table << "daemon" << bool_text(config.daemon) << fort::endr;
    table << "pty" << (runtime.pty == pty_mode_t::unsupported ? "unsupported" :
                       bool_text(runtime.pty == pty_mode_t::enabled)) << fort::endr;
    table << "log_mode" << log_mode_name(config.log_mode) << fort::endr;
    table << "log_dir" << (config.log_dir.empty() ? "<daemon-default>" : config.log_dir) << fort::endr;
    table << "log_file_name" << (config.log_file_name.empty() ? "<derived>" : config.log_file_name) << fort::endr;
    table << "log_max_size_kb" << config.log_max_size_kb << fort::endr;
    table << "log_archive_count" << config.log_archive_count << fort::endr;
    table << "log_paths" << join(runtime.log_paths, " | ") << fort::endr;
    table << "log_degraded" << bool_text(runtime.log_degraded) << fort::endr;
    table << "log_dropped_bytes" << runtime.log_dropped_bytes << fort::endr;
    table << "log_last_error" << (runtime.log_last_error.empty() ? "-" : runtime.log_last_error) << fort::endr;
    table << "log_retry_remaining_ms" << runtime.log_retry_remaining_ms << fort::endr;
    table << "depends_on" << join(config.depends_on, ",") << fort::endr;
    table << "ready" << bool_text(runtime.ready) << fort::endr;
    table << "heartbeat_enabled" << bool_text(runtime.heartbeat_enabled) << fort::endr;
    table << "last_tick_age_ms" << (runtime.has_last_tick_age ?
        std::to_string(runtime.last_tick_age_ms) : "-") << fort::endr;
    table << "uptime_ms" << (runtime.has_uptime ? std::to_string(runtime.uptime_ms) : "-") << fort::endr;
    table << "rss_kib" << (runtime.has_rss ? std::to_string(runtime.rss_kib) : "-") << fort::endr;
    table << "start_timeout" << config.start_timeout << fort::endr;
    table << "failure_action" << failure_action_to_str(config.failure_action) << fort::endr;
    table << "heartbeat_timeout" << config.heartbeat_timeout << fort::endr;
    table << "kill_timeout" << config.kill_timeout_s << fort::endr;
    table << "oom_score_adj" << config.oom_score_adj << fort::endr;
    table << "restart_delay_ms" << config.restart_delay_ms << fort::endr;
    table << "restart_max_delay_ms" << config.restart_max_delay_ms << fort::endr;
    table << "restart_window_ms" << config.restart_window_ms << fort::endr;
    table << "restart_max_attempts" << config.restart_max_attempts << fort::endr;
    table << "restart_reset_after_ms" << config.restart_reset_after_ms << fort::endr;
    table << "restart_count" << runtime.restart_count << fort::endr;
    table << "restart_pending" << bool_text(runtime.restart_pending) << fort::endr;
    table << "restart_delay_remaining_ms" << (runtime.restart_pending ?
        std::to_string(runtime.restart_delay_remaining_ms) : "-") << fort::endr;
    table << "restart_attempts_in_window" << runtime.restart_attempts_in_window << fort::endr;
    table << "restart_suppressed" << bool_text(runtime.restart_suppressed) << fort::endr;
    table << "restart_suppression_reason" << (runtime.restart_suppressed ?
        runtime.restart_suppression_reason : "-") << fort::endr;
    table << "last_exit_reason" << (runtime.has_last_exit ?
        exit_reason_name(runtime.last_exit_reason) : "-") << fort::endr;
    table << "last_exit_code" << (runtime.has_last_exit ?
        std::to_string(runtime.last_exit_code) : "-") << fort::endr;
    table << "process_tree_backend" << (runtime.process_tree_backend.empty() ?
        "-" : runtime.process_tree_backend) << fort::endr;
    table << "process_tree_degraded" << bool_text(runtime.process_tree_degraded) << fort::endr;
    table << "process_tree_degradation_reason" << (runtime.process_tree_degraded ?
        runtime.process_tree_degradation_reason : "-") << fort::endr;
    table << "config_source" << (runtime.config_source.empty() ? "-" : runtime.config_source) << fort::endr;
    return table.to_string() + "\n";
}

} // namespace cli
} // namespace pm_tiny
