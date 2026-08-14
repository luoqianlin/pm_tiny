#include "runtime_snapshot.h"

#include "protocol_v3.h"

namespace pm_tiny {
namespace {

void append_bool(frame_t &frame, bool value) {
    fappend_value<std::int32_t>(frame, value ? 1 : 0);
}

bool read_bool(iframe_stream &stream, const char *field) {
    std::int32_t value = 0;
    stream >> value;
    if (value != 0 && value != 1) throw protocol_error(field);
    return value != 0;
}

} // namespace

const char *exit_reason_name(exit_reason_t reason) {
    switch (reason) {
        case exit_reason_t::none: return "none";
        case exit_reason_t::exited: return "exited";
        case exit_reason_t::signaled: return "signaled";
        case exit_reason_t::unknown: return "unknown";
    }
    return "unknown";
}

process_list_entry make_process_list_entry(const prog_cfg_t &config,
                                           const runtime_snapshot &runtime) {
    process_list_entry entry;
    entry.pid = runtime.pid;
    entry.name = config.name;
    entry.cwd = config.cwd;
    entry.executable = config.executable;
    entry.args = config.args;
    entry.restart_count = runtime.restart_count;
    entry.state = runtime.state;
    entry.has_uptime = runtime.has_uptime;
    entry.uptime_ms = runtime.uptime_ms;
    entry.has_rss = runtime.has_rss;
    entry.rss_kib = runtime.rss_kib;
    entry.daemon = config.daemon;
    entry.pty = runtime.pty;
    entry.depends_on = config.depends_on;
    entry.restart_pending = runtime.restart_pending;
    entry.restart_delay_remaining_ms = runtime.restart_delay_remaining_ms;
    entry.restart_attempts_in_window = runtime.restart_attempts_in_window;
    entry.restart_suppressed = runtime.restart_suppressed;
    entry.restart_suppression_reason = runtime.restart_suppression_reason;
    entry.generation = runtime.generation;
    entry.ready = runtime.ready;
    entry.heartbeat_enabled = runtime.heartbeat_enabled;
    entry.has_last_exit = runtime.has_last_exit;
    entry.last_exit_reason = exit_reason_name(runtime.last_exit_reason);
    entry.last_exit_code = runtime.last_exit_code;
    entry.process_tree_backend = runtime.process_tree_backend;
    entry.process_tree_degraded = runtime.process_tree_degraded;
    entry.process_tree_degradation_reason = runtime.process_tree_degradation_reason;
    entry.config_source = runtime.config_source;
    entry.log_degraded = runtime.log_degraded;
    entry.log_dropped_bytes = runtime.log_dropped_bytes;
    entry.log_last_error = runtime.log_last_error;
    entry.log_retry_remaining_ms = runtime.log_retry_remaining_ms;
    entry.log_paths = runtime.log_paths;
    return entry;
}

void append_runtime_snapshot(frame_t &frame, const runtime_snapshot &snapshot) {
    fappend_value<std::int64_t>(frame, snapshot.pid);
    fappend_value<std::uint64_t>(frame, snapshot.generation);
    fappend_value<std::int32_t>(frame, snapshot.state);
    fappend_value<std::int32_t>(frame, snapshot.restart_count);
    append_bool(frame, snapshot.ready);
    append_bool(frame, snapshot.heartbeat_enabled);
    append_bool(frame, snapshot.has_last_tick_age);
    fappend_value<std::int64_t>(frame, snapshot.last_tick_age_ms);
    append_bool(frame, snapshot.has_uptime);
    fappend_value<std::int64_t>(frame, snapshot.uptime_ms);
    append_bool(frame, snapshot.has_rss);
    fappend_value<std::int64_t>(frame, snapshot.rss_kib);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(snapshot.pty));
    append_bool(frame, snapshot.restart_pending);
    fappend_value<std::int64_t>(frame, snapshot.restart_delay_remaining_ms);
    fappend_value<std::int32_t>(frame, snapshot.restart_attempts_in_window);
    append_bool(frame, snapshot.restart_suppressed);
    fappend_value(frame, snapshot.restart_suppression_reason);
    append_bool(frame, snapshot.has_last_exit);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(snapshot.last_exit_reason));
    fappend_value<std::int32_t>(frame, snapshot.last_exit_code);
    fappend_value(frame, snapshot.process_tree_backend);
    append_bool(frame, snapshot.process_tree_degraded);
    fappend_value(frame, snapshot.process_tree_degradation_reason);
    fappend_value(frame, snapshot.config_source);
    append_bool(frame, snapshot.log_degraded);
    fappend_value<std::uint64_t>(frame, snapshot.log_dropped_bytes);
    fappend_value(frame, snapshot.log_last_error);
    fappend_value<std::int64_t>(frame, snapshot.log_retry_remaining_ms);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(snapshot.log_paths.size()));
    for (const auto &path : snapshot.log_paths) fappend_value(frame, path);
}

runtime_snapshot read_runtime_snapshot(iframe_stream &stream) {
    runtime_snapshot snapshot;
    stream >> snapshot.pid >> snapshot.generation >> snapshot.state >> snapshot.restart_count;
    snapshot.ready = read_bool(stream, "invalid ready flag");
    snapshot.heartbeat_enabled = read_bool(stream, "invalid heartbeat-enabled flag");
    snapshot.has_last_tick_age = read_bool(stream, "invalid last-tick availability flag");
    stream >> snapshot.last_tick_age_ms;
    snapshot.has_uptime = read_bool(stream, "invalid uptime availability flag");
    stream >> snapshot.uptime_ms;
    snapshot.has_rss = read_bool(stream, "invalid RSS availability flag");
    stream >> snapshot.rss_kib;
    std::int32_t pty = 0;
    stream >> pty;
    if (pty < -1 || pty > 1) throw protocol_error("invalid PTY mode");
    snapshot.pty = static_cast<pty_mode_t>(pty);
    snapshot.restart_pending = read_bool(stream, "invalid restart-pending flag");
    stream >> snapshot.restart_delay_remaining_ms >> snapshot.restart_attempts_in_window;
    snapshot.restart_suppressed = read_bool(stream, "invalid restart-suppressed flag");
    stream >> snapshot.restart_suppression_reason;
    snapshot.has_last_exit = read_bool(stream, "invalid last-exit availability flag");
    std::int32_t exit_reason = 0;
    stream >> exit_reason >> snapshot.last_exit_code;
    if (exit_reason < static_cast<std::int32_t>(exit_reason_t::none) ||
        exit_reason > static_cast<std::int32_t>(exit_reason_t::unknown))
        throw protocol_error("invalid last-exit reason");
    snapshot.last_exit_reason = static_cast<exit_reason_t>(exit_reason);
    stream >> snapshot.process_tree_backend;
    snapshot.process_tree_degraded = read_bool(stream, "invalid process-tree degraded flag");
    stream >> snapshot.process_tree_degradation_reason >> snapshot.config_source;
    snapshot.log_degraded = read_bool(stream, "invalid log-degraded flag");
    std::int32_t log_path_count = 0;
    stream >> snapshot.log_dropped_bytes >> snapshot.log_last_error >> snapshot.log_retry_remaining_ms
           >> log_path_count;
    if (log_path_count < 0 || log_path_count > 100) throw protocol_error("invalid log path count");
    snapshot.log_paths.resize(static_cast<std::size_t>(log_path_count));
    for (auto &path : snapshot.log_paths) stream >> path;
    if ((snapshot.has_last_tick_age && snapshot.last_tick_age_ms < 0) ||
        (snapshot.has_uptime && snapshot.uptime_ms < 0) ||
        (snapshot.has_rss && snapshot.rss_kib < 0) ||
        snapshot.restart_delay_remaining_ms < 0 || snapshot.restart_attempts_in_window < 0 ||
        snapshot.log_retry_remaining_ms < 0)
        throw protocol_error("invalid runtime snapshot value");
    return snapshot;
}

void append_inspect_snapshot(frame_t &frame, const inspect_snapshot &snapshot) {
    fappend_value<std::int32_t>(frame, inspect_schema_version);
    append_prog_cfg(frame, snapshot.config);
    append_runtime_snapshot(frame, snapshot.runtime);
}

inspect_snapshot read_inspect_snapshot(iframe_stream &stream) {
    std::int32_t schema = 0;
    stream >> schema;
    if (schema != inspect_schema_version) throw protocol_error("unsupported inspect schema version");
    inspect_snapshot snapshot;
    snapshot.config = read_prog_cfg(stream);
    snapshot.runtime = read_runtime_snapshot(stream);
    return snapshot;
}

} // namespace pm_tiny
