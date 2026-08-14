#ifndef PM_TINY_RUNTIME_SNAPSHOT_H
#define PM_TINY_RUNTIME_SNAPSHOT_H

#include "process_list.h"
#include "prog_cfg.h"

#include <cstdint>
#include <string>

namespace pm_tiny {

constexpr std::int32_t inspect_schema_version = 2;

enum class exit_reason_t : std::int32_t {
    none = 0,
    exited = 1,
    signaled = 2,
    unknown = 3,
};

struct runtime_snapshot {
    std::int64_t pid = -1;
    std::uint64_t generation = 0;
    std::int32_t state = 0;
    std::int32_t restart_count = 0;
    bool ready = false;
    bool heartbeat_enabled = false;
    bool has_last_tick_age = false;
    std::int64_t last_tick_age_ms = 0;
    bool has_uptime = false;
    std::int64_t uptime_ms = 0;
    bool has_rss = false;
    std::int64_t rss_kib = 0;
    pty_mode_t pty = pty_mode_t::unsupported;
    bool restart_pending = false;
    std::int64_t restart_delay_remaining_ms = 0;
    std::int32_t restart_attempts_in_window = 0;
    bool restart_suppressed = false;
    std::string restart_suppression_reason;
    bool has_last_exit = false;
    exit_reason_t last_exit_reason = exit_reason_t::none;
    std::int32_t last_exit_code = 0;
    std::string process_tree_backend;
    bool process_tree_degraded = false;
    std::string process_tree_degradation_reason;
    std::string config_source;
    bool log_degraded = false;
    std::uint64_t log_dropped_bytes = 0;
    std::string log_last_error;
    std::int64_t log_retry_remaining_ms = 0;
    std::vector<std::string> log_paths;
};

struct inspect_snapshot {
    prog_cfg_t config;
    runtime_snapshot runtime;
};

process_list_entry make_process_list_entry(const prog_cfg_t &config,
                                           const runtime_snapshot &runtime);
void append_runtime_snapshot(frame_t &frame, const runtime_snapshot &snapshot);
runtime_snapshot read_runtime_snapshot(iframe_stream &stream);
void append_inspect_snapshot(frame_t &frame, const inspect_snapshot &snapshot);
inspect_snapshot read_inspect_snapshot(iframe_stream &stream);
const char *exit_reason_name(exit_reason_t reason);

} // namespace pm_tiny

#endif
