#ifndef PM_TINY_PROCESS_LIST_H
#define PM_TINY_PROCESS_LIST_H

#include <cstdint>
#include <string>
#include <vector>

#include "frame_stream.hpp"

namespace pm_tiny {

constexpr std::int32_t process_list_schema_version = 5;

enum class pty_mode_t : std::int32_t {
    unsupported = -1,
    disabled = 0,
    enabled = 1,
};

struct process_list_entry {
    std::int64_t pid = -1;
    std::string name;
    std::string cwd;
    std::string executable;
    std::vector<std::string> args;
    std::int32_t restart_count = 0;
    std::int32_t state = 0;
    bool has_uptime = false;
    std::int64_t uptime_ms = 0;
    bool has_rss = false;
    std::int64_t rss_kib = 0;
    bool daemon = false;
    pty_mode_t pty = pty_mode_t::unsupported;
    std::vector<std::string> depends_on;
    bool restart_pending = false;
    std::int64_t restart_delay_remaining_ms = 0;
    std::int32_t restart_attempts_in_window = 0;
    bool restart_suppressed = false;
    std::string restart_suppression_reason;
    std::uint64_t generation = 0;
    bool ready = false;
    bool heartbeat_enabled = false;
    bool has_last_exit = false;
    std::string last_exit_reason;
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

void append_process_list(frame_t &frame, const std::vector<process_list_entry> &entries);
std::vector<process_list_entry> read_process_list(iframe_stream &stream);

} // namespace pm_tiny

#endif
