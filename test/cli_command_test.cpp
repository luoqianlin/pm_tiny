#include "cli_command.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

pm_tiny::cli::command_parse_result parse(std::initializer_list<const char *> args) {
    std::vector<std::string> values;
    for (const auto *arg : args) values.emplace_back(arg);
    return pm_tiny::cli::parse_command_line(values);
}

} // namespace

int main() {
    using pm_tiny::cli::command_kind;

    auto list = parse({"pm", "status", "--json", "--no-color"});
    expect(list.success && list.command.kind == command_kind::list, "status alias should parse as list");
    expect(list.command.list_options.json && list.command.list_options.no_color, "list options should parse");
    expect(!parse({"pm", "list", "--wide", "--json"}).success, "wide and json should conflict");

    auto graph = parse({"pm", "graph", "--no-color", "api", "--json"});
    expect(graph.success && graph.command.kind == command_kind::graph,
           "graph command should parse");
    expect(graph.command.graph_options.focus == "api" && graph.command.graph_options.json &&
           graph.command.graph_options.no_color, "graph options should parse in any order");
    expect(parse({"pm", "dag", "worker", "--dot"}).command.kind == command_kind::graph,
           "dag alias should parse");
    expect(!parse({"pm", "graph", "--json", "--dot"}).success,
           "graph output formats should conflict");
    expect(!parse({"pm", "graph", "api", "worker"}).success,
           "graph should accept at most one focus");
    expect(!parse({"pm", "graph", "--unknown"}).success,
           "unknown graph options should fail");

    auto restart = parse({"pm", "restart", "app", "--log"});
    expect(restart.success && restart.command.name == "app" && restart.command.show_log,
           "restart --log should parse");
    expect(!parse({"pm", "stop"}).success, "named command should require a name");
    expect(!parse({"pm", "save", "extra"}).success, "plain command should reject arguments");

    auto start = parse({"pm", "start", "./server --flag", "--name", "api", "--kill_timeout", "5",
                        "--depends_on", " db, cache ,,", "--failure_action", "restart",
                        "--heartbeat_timeout", "9", "--env_var", "A=B", "--no_pty", "--log"});
    expect(start.success && start.command.kind == command_kind::start, "start should parse");
    expect(start.command.start.name == "api" && start.command.start.kill_timeout_sec == 5,
           "start scalar options should parse");
    expect(start.command.start.depends_on == std::vector<std::string>({"db", "cache"}),
           "dependencies should be trimmed");
    expect(start.command.start.failure_action == pm_tiny::failure_action_t::RESTART,
           "failure action should parse");
    expect(!start.command.start.pty && start.command.start.show_log, "start flags should parse");
    expect(!parse({"pm", "start", "app", "--kill_timeout", "invalid"}).success,
           "invalid integer should fail");
    expect(!parse({"pm", "start", "app", "--unknown"}).success, "unknown start option should fail");

    expect(pm_tiny::cli::command_protocol_type(command_kind::restart) == 0x28,
           "restart protocol mapping should be shared");
    expect(pm_tiny::cli::command_protocol_type(command_kind::graph) == 0x23,
           "graph should reuse the process-list protocol");
    expect(parse({"pm", "shutdown"}).command.kind == command_kind::quit, "shutdown alias should parse");
    expect(parse({"pm", "-V"}).command.kind == command_kind::version, "version option should be case insensitive");
    return 0;
}
