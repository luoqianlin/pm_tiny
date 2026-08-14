#include "daemon_info.h"

#include <set>
#include <stdexcept>

namespace pm_tiny {
namespace {

void append_bool(frame_t &frame, bool value) { fappend_value<std::uint8_t>(frame, value ? 1 : 0); }

bool read_bool(iframe_stream &stream) {
    std::uint8_t value = 0;
    stream >> value;
    if (value > 1) throw protocol_error("invalid daemon-info boolean");
    return value != 0;
}

template <typename Enum>
Enum read_enum(iframe_stream &stream, std::uint8_t maximum, const char *error) {
    std::uint8_t value = 0;
    stream >> value;
    if (value > maximum) throw protocol_error(error);
    return static_cast<Enum>(value);
}

template <typename T>
void append_vector(frame_t &frame, const std::vector<T> &values) {
    if (values.size() > daemon_info_max_items) throw protocol_error("daemon-info item count exceeds limit");
    fappend_value<std::uint32_t>(frame, static_cast<std::uint32_t>(values.size()));
    for (const auto &value : values) fappend_value(frame, value);
}

template <typename T>
std::vector<T> read_vector(iframe_stream &stream) {
    std::uint32_t count = 0;
    stream >> count;
    if (count > daemon_info_max_items) throw protocol_error("invalid daemon-info item count");
    std::vector<T> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        T value{};
        stream >> value;
        result.push_back(std::move(value));
    }
    return result;
}

const std::set<std::string> &source_fields() {
    static const std::set<std::string> fields = {
        "config_file", "home_dir", "pid_file", "program_config_file", "app_environ_dir",
        "app_log_dir", "daemon_log_file", "uds_address", "uds_abstract_namespace",
        "allowed_uids", "allowed_gids", "named_pipe", "pipe_sddl", "service_name",
        "requested_process_tree_mode", "cgroup_root", "log_level", "log_max_size_kb",
        "log_archive_count"
    };
    return fields;
}

} // namespace

const char *daemon_info_platform_name(daemon_info_platform value) {
    switch (value) {
        case daemon_info_platform::linux_os: return "linux";
        case daemon_info_platform::android_os: return "android";
        case daemon_info_platform::windows_os: return "windows";
    }
    return "unknown";
}

const char *daemon_run_mode_name(daemon_run_mode value) {
    switch (value) {
        case daemon_run_mode::foreground: return "foreground";
        case daemon_run_mode::daemon: return "daemon";
        case daemon_run_mode::service: return "service";
    }
    return "unknown";
}

const char *daemon_runtime_state_name(daemon_runtime_state value) {
    switch (value) {
        case daemon_runtime_state::running: return "running";
        case daemon_runtime_state::reloading: return "reloading";
        case daemon_runtime_state::stopping: return "stopping";
    }
    return "unknown";
}

const char *daemon_log_sink_name(daemon_log_sink value) {
    switch (value) {
        case daemon_log_sink::file: return "file";
        case daemon_log_sink::console: return "console";
        case daemon_log_sink::console_fallback: return "console_fallback";
    }
    return "unknown";
}

void append_daemon_info(frame_t &f, const daemon_info_snapshot &s) {
    fappend_value<std::int32_t>(f, daemon_info_schema_version);
    fappend_value(f, s.version); fappend_value(f, s.protocol_version);
    fappend_value<std::uint8_t>(f, static_cast<std::uint8_t>(s.platform));
    fappend_value(f, s.pid); fappend_value(f, s.uptime_ms);
    fappend_value<std::uint8_t>(f, static_cast<std::uint8_t>(s.run_mode));
    fappend_value<std::uint8_t>(f, static_cast<std::uint8_t>(s.state));
    append_bool(f, s.single_threaded_control_loop); append_bool(f, s.persistence_active);
    fappend_value(f, s.file_config_count); fappend_value(f, s.runtime_definition_count);
    fappend_value(f, s.config_file); append_bool(f, s.config_loaded); fappend_value(f, s.home_dir);
    fappend_value(f, s.pid_file); fappend_value(f, s.program_config_file);
    fappend_value(f, s.app_environ_dir); fappend_value(f, s.app_log_dir); fappend_value(f, s.daemon_log_file);
    fappend_value(f, s.uds_address); append_bool(f, s.uds_abstract_namespace);
    append_vector(f, s.allowed_uids); append_vector(f, s.allowed_gids);
    fappend_value(f, s.named_pipe); fappend_value(f, s.pipe_sddl); fappend_value(f, s.service_name);
    fappend_value(f, s.requested_process_tree_mode); fappend_value(f, s.effective_process_tree_mode);
    fappend_value(f, s.cgroup_root); append_bool(f, s.subreaper_enabled);
    append_bool(f, s.process_tree_degraded); fappend_value(f, s.process_tree_degradation_reason);
    fappend_value(f, s.log_level); fappend_value(f, s.log_max_size_kb); fappend_value(f, s.log_archive_count);
    append_bool(f, s.log_console_mirror); fappend_value<std::uint8_t>(f, static_cast<std::uint8_t>(s.log_sink));
    append_bool(f, s.log_degraded); fappend_value(f, s.log_last_error);
    append_bool(f, s.dynamic_create); append_bool(f, s.pty); append_bool(f, s.switch_user);
    append_bool(f, s.oom_adjust); append_bool(f, s.failure_action); append_bool(f, s.service_mode);
    append_vector(f, s.process_tree_backends);
    if (s.sources.size() > daemon_info_max_sources) throw protocol_error("daemon-info source count exceeds limit");
    fappend_value<std::uint32_t>(f, static_cast<std::uint32_t>(s.sources.size()));
    for (const auto &entry : s.sources) {
        if (source_fields().count(entry.first) == 0) throw protocol_error("unknown daemon-info source key");
        fappend_value(f, entry.first);
        fappend_value<std::uint8_t>(f, static_cast<std::uint8_t>(entry.second));
    }
}

daemon_info_snapshot read_daemon_info(iframe_stream &stream) {
    std::int32_t schema = 0;
    stream >> schema;
    if (schema != daemon_info_schema_version) throw protocol_error("unsupported daemon-info schema");
    daemon_info_snapshot s;
    stream >> s.version >> s.protocol_version;
    s.platform = read_enum<daemon_info_platform>(stream, 2, "invalid daemon-info platform");
    stream >> s.pid >> s.uptime_ms;
    s.run_mode = read_enum<daemon_run_mode>(stream, 2, "invalid daemon-info run mode");
    s.state = read_enum<daemon_runtime_state>(stream, 2, "invalid daemon-info runtime state");
    s.single_threaded_control_loop = read_bool(stream); s.persistence_active = read_bool(stream);
    stream >> s.file_config_count >> s.runtime_definition_count;
    stream >> s.config_file; s.config_loaded = read_bool(stream); stream >> s.home_dir >> s.pid_file;
    stream >> s.program_config_file >> s.app_environ_dir >> s.app_log_dir >> s.daemon_log_file;
    stream >> s.uds_address; s.uds_abstract_namespace = read_bool(stream);
    s.allowed_uids = read_vector<std::uint32_t>(stream); s.allowed_gids = read_vector<std::uint32_t>(stream);
    stream >> s.named_pipe >> s.pipe_sddl >> s.service_name;
    stream >> s.requested_process_tree_mode >> s.effective_process_tree_mode >> s.cgroup_root;
    s.subreaper_enabled = read_bool(stream); s.process_tree_degraded = read_bool(stream);
    stream >> s.process_tree_degradation_reason >> s.log_level >> s.log_max_size_kb >> s.log_archive_count;
    s.log_console_mirror = read_bool(stream);
    s.log_sink = read_enum<daemon_log_sink>(stream, 2, "invalid daemon-info log sink");
    s.log_degraded = read_bool(stream); stream >> s.log_last_error;
    s.dynamic_create = read_bool(stream); s.pty = read_bool(stream); s.switch_user = read_bool(stream);
    s.oom_adjust = read_bool(stream); s.failure_action = read_bool(stream); s.service_mode = read_bool(stream);
    s.process_tree_backends = read_vector<std::string>(stream);
    std::uint32_t source_count = 0;
    stream >> source_count;
    if (source_count > daemon_info_max_sources) throw protocol_error("invalid daemon-info source count");
    for (std::uint32_t i = 0; i < source_count; ++i) {
        std::string key;
        stream >> key;
        if (source_fields().count(key) == 0) throw protocol_error("unknown daemon-info source key");
        if (s.sources.count(key) != 0) throw protocol_error("duplicate daemon-info source key");
        s.sources[key] = read_enum<daemon_config_source>(stream, 4, "invalid daemon-info config source");
    }
    if (stream.remaining_size() != 0) throw protocol_error("trailing daemon-info payload");
    return s;
}

daemon_info_snapshot make_daemon_info_base(const daemon_config &config,
                                           const daemon_cli_options &options,
                                           daemon_info_platform platform,
                                           const std::string &version,
                                           std::int64_t pid,
                                           std::int64_t uptime_ms) {
    daemon_info_snapshot s;
    s.version = version;
    s.platform = platform;
    s.pid = pid;
    s.uptime_ms = uptime_ms;
    s.run_mode = options.service ? daemon_run_mode::service :
                 options.daemonize ? daemon_run_mode::daemon : daemon_run_mode::foreground;
    s.config_file = config.config_path; s.config_loaded = config.config_loaded;
    s.home_dir = config.home_dir; s.pid_file = config.lock_file;
    s.program_config_file = config.program_config_file;
    s.app_environ_dir = config.app_environ_dir; s.app_log_dir = config.app_log_dir;
    s.daemon_log_file = config.log_file;
    s.uds_address = config.socket_file; s.uds_abstract_namespace = config.uds_abstract_namespace;
    s.allowed_uids.assign(config.allowed_uids.begin(), config.allowed_uids.end());
    s.allowed_gids.assign(config.allowed_gids.begin(), config.allowed_gids.end());
    s.named_pipe = config.pipe_name; s.pipe_sddl = config.pipe_sddl; s.service_name = options.service_name;
    s.requested_process_tree_mode = config.process_tree_mode; s.cgroup_root = config.cgroup_root;
    s.log_level = config.log_level; s.log_max_size_kb = config.log_max_size_kb;
    s.log_archive_count = config.log_archive_count;
    const std::pair<const char *, const char *> fields[] = {
        {"config_file", "config_path"}, {"home_dir", "home_dir"}, {"pid_file", "lock_file"},
        {"program_config_file", "program_config_file"}, {"app_environ_dir", "app_environ_dir"},
        {"app_log_dir", "app_log_dir"}, {"daemon_log_file", "log_file"},
        {"uds_address", "socket_file"}, {"uds_abstract_namespace", "uds_abstract_namespace"},
        {"allowed_uids", "allowed_uids"}, {"allowed_gids", "allowed_gids"},
        {"named_pipe", "pipe_name"}, {"pipe_sddl", "pipe_sddl"},
        {"requested_process_tree_mode", "process_tree_mode"}, {"cgroup_root", "cgroup_root"},
        {"log_level", "log_level"}, {"log_max_size_kb", "log_max_size_kb"},
        {"log_archive_count", "log_archive_count"}
    };
    for (const auto &field : fields) s.sources[field.first] = config.source_of(field.second);
    s.sources["service_name"] = !options.service_name_explicit
        ? daemon_config_source::default_value : daemon_config_source::command_line;
    return s;
}

} // namespace pm_tiny
