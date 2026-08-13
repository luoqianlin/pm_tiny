#include "process_list.h"

#include <limits>
#include <stdexcept>

namespace pm_tiny {
namespace {

constexpr std::int32_t max_process_count = 100000;
constexpr std::int32_t max_dependency_count = 100000;

void append_bool(frame_t &frame, bool value) {
    fappend_value<std::int32_t>(frame, value ? 1 : 0);
}

bool read_bool(iframe_stream &stream, const char *field) {
    std::int32_t value = 0;
    stream >> value;
    if (value != 0 && value != 1) {
        throw protocol_error(field);
    }
    return value != 0;
}

} // namespace

void append_process_list(frame_t &frame, const std::vector<process_list_entry> &entries) {
    if (entries.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("process list is too large");
    }
    fappend_value<std::int32_t>(frame, process_list_schema_version);
    fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(entries.size()));
    for (const auto &entry : entries) {
        fappend_value<std::int64_t>(frame, entry.pid);
        fappend_value(frame, entry.name);
        fappend_value(frame, entry.cwd);
        fappend_value(frame, entry.command);
        fappend_value<std::int32_t>(frame, entry.restart_count);
        fappend_value<std::int32_t>(frame, entry.state);
        append_bool(frame, entry.has_uptime);
        fappend_value<std::int64_t>(frame, entry.uptime_ms);
        append_bool(frame, entry.has_rss);
        fappend_value<std::int64_t>(frame, entry.rss_kib);
        append_bool(frame, entry.daemon);
        fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(entry.pty));
        if (entry.depends_on.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::length_error("dependency list is too large");
        }
        fappend_value<std::int32_t>(frame, static_cast<std::int32_t>(entry.depends_on.size()));
        for (const auto &dependency : entry.depends_on) {
            fappend_value(frame, dependency);
        }
        append_bool(frame, entry.restart_pending);
        fappend_value<std::int64_t>(frame, entry.restart_delay_remaining_ms);
        fappend_value<std::int32_t>(frame, entry.restart_attempts_in_window);
        append_bool(frame, entry.restart_suppressed);
        fappend_value(frame, entry.restart_suppression_reason);
    }
}

std::vector<process_list_entry> read_process_list(iframe_stream &stream) {
    std::int32_t schema_version = 0;
    stream >> schema_version;
    if (schema_version != process_list_schema_version) {
        throw protocol_error("unsupported process-list schema version");
    }
    std::int32_t count = 0;
    stream >> count;
    if (count < 0 || count > max_process_count) {
        throw protocol_error("invalid process-list entry count");
    }
    std::vector<process_list_entry> entries(static_cast<std::size_t>(count));
    for (auto &entry : entries) {
        stream >> entry.pid >> entry.name >> entry.cwd >> entry.command;
        stream >> entry.restart_count >> entry.state;
        entry.has_uptime = read_bool(stream, "invalid uptime availability flag");
        stream >> entry.uptime_ms;
        entry.has_rss = read_bool(stream, "invalid RSS availability flag");
        stream >> entry.rss_kib;
        entry.daemon = read_bool(stream, "invalid daemon flag");
        std::int32_t pty = 0;
        stream >> pty;
        if (pty < -1 || pty > 1) {
            throw protocol_error("invalid PTY mode");
        }
        entry.pty = static_cast<pty_mode_t>(pty);
        std::int32_t dependency_count = 0;
        stream >> dependency_count;
        if (dependency_count < 0 || dependency_count > max_dependency_count) {
            throw protocol_error("invalid dependency count");
        }
        entry.depends_on.resize(static_cast<std::size_t>(dependency_count));
        for (auto &dependency : entry.depends_on) {
            stream >> dependency;
        }
        entry.restart_pending = read_bool(stream, "invalid restart-pending flag");
        stream >> entry.restart_delay_remaining_ms >> entry.restart_attempts_in_window;
        if (entry.restart_delay_remaining_ms < 0 || entry.restart_attempts_in_window < 0) {
            throw protocol_error("invalid restart runtime state");
        }
        entry.restart_suppressed = read_bool(stream, "invalid restart-suppressed flag");
        stream >> entry.restart_suppression_reason;
    }
    return entries;
}

} // namespace pm_tiny
