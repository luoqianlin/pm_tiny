#ifndef PM_TINY_CLI_COMMAND_H
#define PM_TINY_CLI_COMMAND_H

#include "pm_tiny_enum.h"
#include "dependency_graph_renderer.h"
#include "process_list_renderer.h"

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
};

struct start_command_options {
    std::string command;
    std::string name;
    int kill_timeout_sec = 3;
    std::string run_as;
    std::vector<std::string> env_vars;
    std::vector<std::string> depends_on;
    int start_timeout = 0;
    failure_action_t failure_action = failure_action_t::SKIP;
    bool daemon = true;
    int heartbeat_timeout = -1;
    int oom_score_adj = 0;
    bool pty = true;
    bool show_log = false;
    bool has_definition_options = false;
};

struct parsed_command {
    command_kind kind = command_kind::help;
    std::string name;
    bool show_log = false;
    list_render_options list_options;
    dependency_graph_render_options graph_options;
    start_command_options start;
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
