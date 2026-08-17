#ifndef PM_TINY_CLI_COMMAND_H
#define PM_TINY_CLI_COMMAND_H

#include "pm_tiny_enum.h"
#include "dependency_graph_renderer.h"
#include "process_list_renderer.h"
#include "program_log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pm_tiny {
namespace cli {

enum class command_kind {
    help,
    list,
    graph,
    start,
    stop,
    restart,
    remove,
    save,
    log,
    inspect,
    reload,
    quit,
    version,
    info,
};

struct start_command_options {
    std::string name;
    std::string cwd;
    std::string executable;
    std::vector<std::string> args;
    int kill_timeout_sec = 3;
    std::string run_as;
    std::vector<std::string> env;
    std::vector<std::string> depends_on;
    int start_timeout = 0;
    failure_action_t failure_action = failure_action_t::SKIP;
    bool daemon = true;
    int heartbeat_timeout = -1;
    int oom_score_adj = 0;
    bool pty = false;
    bool show_log = false;
    log_mode_t log_mode = log_mode_t::split;
    bool log_mode_explicit = false;
    std::string log_dir;
    std::string log_file_name;
    int log_max_size_kb = 4096;
    int log_archive_count = 3;
    bool create = false;
    int restart_delay_ms = 1000;
    int restart_max_delay_ms = 30000;
    int restart_window_ms = 60000;
    int restart_max_attempts = 10;
    int restart_reset_after_ms = 60000;
};

struct parsed_command {
    command_kind kind = command_kind::help;
    std::string name;
    bool show_log = false;
    bool log_history = false;
    bool no_list = false;
    list_render_options list_options;
    dependency_graph_render_options graph_options;
    start_command_options start;
    bool info_json = false;
};

struct command_parse_result {
    bool success = false;
    parsed_command command;
    std::string error;
};

command_parse_result parse_command_line(int argc, char *argv[]);
command_parse_result parse_command_line(const std::vector<std::string> &args);

std::uint16_t command_protocol_type(command_kind kind);
std::string command_usage(const std::string &executable, bool dynamic_start_supported);

} // namespace cli
} // namespace pm_tiny

#endif
