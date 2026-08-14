#pragma once

#include "daemon_config.h"
#include "frame_stream.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pm_tiny {

constexpr std::int32_t daemon_info_schema_version = 1;
constexpr std::uint32_t daemon_info_max_items = 1024;
constexpr std::uint32_t daemon_info_max_sources = 64;

enum class daemon_info_platform : std::uint8_t { linux_os = 0, android_os = 1, windows_os = 2 };
enum class daemon_run_mode : std::uint8_t { foreground = 0, daemon = 1, service = 2 };
enum class daemon_runtime_state : std::uint8_t { running = 0, reloading = 1, stopping = 2 };
enum class daemon_log_sink : std::uint8_t { file = 0, console = 1, console_fallback = 2 };

const char *daemon_info_platform_name(daemon_info_platform value);
const char *daemon_run_mode_name(daemon_run_mode value);
const char *daemon_runtime_state_name(daemon_runtime_state value);
const char *daemon_log_sink_name(daemon_log_sink value);

struct daemon_info_snapshot {
    std::string version;
    std::int32_t protocol_version = 3;
    daemon_info_platform platform = daemon_info_platform::linux_os;
    std::int64_t pid = 0;
    std::int64_t uptime_ms = 0;

    daemon_run_mode run_mode = daemon_run_mode::foreground;
    daemon_runtime_state state = daemon_runtime_state::running;
    bool single_threaded_control_loop = true;
    bool persistence_active = false;
    std::uint32_t file_config_count = 0;
    std::uint32_t runtime_definition_count = 0;

    std::string config_file;
    bool config_loaded = false;
    std::string home_dir;
    std::string pid_file;
    std::string program_config_file;
    std::string app_environ_dir;
    std::string app_log_dir;
    std::string daemon_log_file;

    std::string uds_address;
    bool uds_abstract_namespace = false;
    std::vector<std::uint32_t> allowed_uids;
    std::vector<std::uint32_t> allowed_gids;
    std::string named_pipe;
    std::string pipe_sddl;
    std::string service_name;

    std::string requested_process_tree_mode;
    std::string effective_process_tree_mode;
    std::string cgroup_root;
    bool subreaper_enabled = false;
    bool process_tree_degraded = false;
    std::string process_tree_degradation_reason;

    std::string log_level;
    std::int32_t log_max_size_kb = 0;
    std::int32_t log_archive_count = 0;
    bool log_console_mirror = false;
    daemon_log_sink log_sink = daemon_log_sink::console;
    bool log_degraded = false;
    std::string log_last_error;

    bool dynamic_create = true;
    bool pty = false;
    bool switch_user = false;
    bool oom_adjust = false;
    bool failure_action = false;
    bool service_mode = false;
    std::vector<std::string> process_tree_backends;

    std::map<std::string, daemon_config_source> sources;
};

void append_daemon_info(frame_t &frame, const daemon_info_snapshot &snapshot);
daemon_info_snapshot read_daemon_info(iframe_stream &stream);
daemon_info_snapshot make_daemon_info_base(const daemon_config &config,
                                           const daemon_cli_options &options,
                                           daemon_info_platform platform,
                                           const std::string &version,
                                           std::int64_t pid,
                                           std::int64_t uptime_ms);

} // namespace pm_tiny
