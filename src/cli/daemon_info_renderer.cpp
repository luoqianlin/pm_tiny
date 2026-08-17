#include "daemon_info_renderer.h"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>

namespace pm_tiny { namespace cli {
namespace {

using json = nlohmann::json;

std::string source(const daemon_info_snapshot &s, const std::string &field) {
    const auto iter = s.sources.find(field);
    return daemon_config_source_name(iter == s.sources.end()
        ? daemon_config_source::default_value : iter->second);
}

template <typename T>
json sourced(const daemon_info_snapshot &s, const std::string &field, const T &value) {
    return json{{"value", value}, {"source", source(s, field)}};
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

template <typename T>
std::string joined(const std::vector<T> &values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ", ";
        out << values[i];
    }
    return out.str();
}

void row(std::ostringstream &out, const daemon_info_snapshot &s,
         const std::string &field, const std::string &value) {
    out << "  " << std::left << std::setw(30) << field
        << " | " << value << " | " << source(s, field) << '\n';
}

} // namespace

std::string render_daemon_info(const daemon_info_snapshot &s, bool as_json) {
    if (as_json) {
        json root;
        root["schema_version"] = daemon_info_schema_version;
        root["identity"] = {{"version", s.version}, {"protocol_version", s.protocol_version},
                            {"platform", daemon_info_platform_name(s.platform)},
                            {"pid", s.pid}, {"uptime_ms", s.uptime_ms}};
        root["runtime"] = {{"mode", daemon_run_mode_name(s.run_mode)},
                           {"state", daemon_runtime_state_name(s.state)},
                           {"control_event_loop", s.single_threaded_control_loop ? "single_threaded" : "multi_threaded"},
                           {"persistence_active", s.persistence_active},
                           {"file_config_count", s.file_config_count},
                           {"runtime_definition_count", s.runtime_definition_count}};
        root["config"] = {
            {"config_file", sourced(s, "config_file", s.config_file)},
            {"config_loaded", s.config_loaded},
            {"home_dir", sourced(s, "home_dir", s.home_dir)},
            {"pid_file", sourced(s, "pid_file", s.pid_file)},
            {"program_config_file", sourced(s, "program_config_file", s.program_config_file)},
            {"app_environ_dir", sourced(s, "app_environ_dir", s.app_environ_dir)},
            {"app_log_dir", sourced(s, "app_log_dir", s.app_log_dir)},
            {"daemon_log_file", sourced(s, "daemon_log_file", s.daemon_log_file)}};
        if (s.platform == daemon_info_platform::windows_os) {
            root["ipc"] = {
                {"named_pipe", sourced(s, "named_pipe", s.named_pipe)},
                {"pipe_sddl", sourced(s, "pipe_sddl", s.pipe_sddl)},
                {"service_name", sourced(s, "service_name", s.service_name)}};
        } else {
            root["ipc"] = {
                {"uds_address", sourced(s, "uds_address", s.uds_address)},
                {"uds_abstract_namespace", sourced(s, "uds_abstract_namespace", s.uds_abstract_namespace)},
                {"allowed_uids", sourced(s, "allowed_uids", s.allowed_uids)},
                {"allowed_gids", sourced(s, "allowed_gids", s.allowed_gids)}};
        }
        root["logging"] = {
            {"level", sourced(s, "log_level", s.log_level)},
            {"max_size_kb", sourced(s, "log_max_size_kb", s.log_max_size_kb)},
            {"archive_count", sourced(s, "log_archive_count", s.log_archive_count)},
            {"console_mirror", s.log_console_mirror}, {"sink", daemon_log_sink_name(s.log_sink)},
            {"degraded", s.log_degraded}, {"last_error", s.log_last_error}};
        if (s.platform == daemon_info_platform::windows_os) {
            root["process_tree"] = {{"effective_mode", s.effective_process_tree_mode},
                                    {"degraded", s.process_tree_degraded},
                                    {"degradation_reason", s.process_tree_degradation_reason}};
        } else {
            root["process_tree"] = {
                {"requested_mode", sourced(s, "requested_process_tree_mode", s.requested_process_tree_mode)},
                {"effective_mode", s.effective_process_tree_mode},
                {"cgroup_root", sourced(s, "cgroup_root", s.cgroup_root)},
                {"subreaper_enabled", s.subreaper_enabled}, {"degraded", s.process_tree_degraded},
                {"degradation_reason", s.process_tree_degradation_reason}};
        }
        root["capabilities"] = {
            {"dynamic_create", s.dynamic_create}, {"pty", s.pty}, {"switch_user", s.switch_user},
            {"oom_adjust", s.oom_adjust}, {"failure_action", s.failure_action},
            {"service_mode", s.service_mode}, {"process_tree_backends", s.process_tree_backends}};
        return root.dump(2) + "\n";
    }

    std::ostringstream out;
    out << "Identity\n"
        << "  version: " << s.version << '\n'
        << "  protocol: " << s.protocol_version << '\n'
        << "  platform: " << daemon_info_platform_name(s.platform) << '\n'
        << "  pid: " << s.pid << '\n'
        << "  uptime_ms: " << s.uptime_ms << "\n\n"
        << "Runtime\n"
        << "  mode: " << daemon_run_mode_name(s.run_mode) << '\n'
        << "  state: " << daemon_runtime_state_name(s.state) << '\n'
        << "  control_event_loop: single_threaded\n"
        << "  persistence_active: " << bool_text(s.persistence_active) << '\n'
        << "  definitions: file=" << s.file_config_count << ", runtime=" << s.runtime_definition_count << "\n\n"
        << "Configuration\n"
        << "  " << std::left << std::setw(30) << "field" << " | value | source\n";
    row(out, s, "config_file", s.config_file);
    row(out, s, "home_dir", s.home_dir); row(out, s, "pid_file", s.pid_file);
    row(out, s, "program_config_file", s.program_config_file);
    row(out, s, "app_environ_dir", s.app_environ_dir); row(out, s, "app_log_dir", s.app_log_dir);
    row(out, s, "daemon_log_file", s.daemon_log_file);
    if (s.platform == daemon_info_platform::windows_os) {
        row(out, s, "named_pipe", s.named_pipe); row(out, s, "pipe_sddl", s.pipe_sddl);
        row(out, s, "service_name", s.service_name);
    } else {
        row(out, s, "uds_address", s.uds_address);
        row(out, s, "uds_abstract_namespace", bool_text(s.uds_abstract_namespace));
        row(out, s, "allowed_uids", joined(s.allowed_uids)); row(out, s, "allowed_gids", joined(s.allowed_gids));
        row(out, s, "requested_process_tree_mode", s.requested_process_tree_mode);
        row(out, s, "cgroup_root", s.cgroup_root);
    }
    row(out, s, "log_level", s.log_level);
    row(out, s, "log_max_size_kb", std::to_string(s.log_max_size_kb));
    row(out, s, "log_archive_count", std::to_string(s.log_archive_count));
    out << "\nIPC\n  endpoint: " << (s.platform == daemon_info_platform::windows_os ? s.named_pipe : s.uds_address)
        << "\n\nLogging\n  sink: " << daemon_log_sink_name(s.log_sink)
        << "\n  console_mirror: " << bool_text(s.log_console_mirror)
        << "\n  log_degraded: " << bool_text(s.log_degraded)
        << "\n  last_error: " << (s.log_last_error.empty() ? "-" : s.log_last_error)
        << "\n\nProcess tree\n  effective_mode: " << s.effective_process_tree_mode
        << "\n  subreaper_enabled: " << bool_text(s.subreaper_enabled)
        << "\n  process_tree_degraded: " << bool_text(s.process_tree_degraded)
        << "\n  degradation_reason: " << (s.process_tree_degradation_reason.empty() ? "-" : s.process_tree_degradation_reason)
        << "\n\nCapabilities\n"
        << "  dynamic_create: " << bool_text(s.dynamic_create) << '\n'
        << "  pty: " << bool_text(s.pty) << '\n'
        << "  switch_user: " << bool_text(s.switch_user) << '\n'
        << "  oom_adjust: " << bool_text(s.oom_adjust) << '\n'
        << "  failure_action: " << bool_text(s.failure_action) << '\n'
        << "  service_mode: " << bool_text(s.service_mode) << '\n'
        << "  process_tree_backends: " << joined(s.process_tree_backends) << '\n';
    return out.str();
}

} } // namespace pm_tiny::cli
