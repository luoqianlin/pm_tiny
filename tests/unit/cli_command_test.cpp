#include "cli_command.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << message << std::endl; std::abort(); }
}
pm_tiny::cli::command_parse_result parse(std::initializer_list<const char *> args) {
    std::vector<std::string> values;
    for (const auto *arg : args) values.emplace_back(arg);
    return pm_tiny::cli::parse_command_line(values);
}
}

int main() {
    using pm_tiny::cli::command_kind;
    auto list = parse({"pm", "status", "--json", "--no-color"});
    expect(list.success && list.command.kind == command_kind::list, "status alias should parse as list");
    expect(list.command.list_options.json && list.command.list_options.no_color, "list options should parse");
    expect(!parse({"pm", "list", "--wide", "--json"}).success, "wide and json should conflict");
    auto info = parse({"pm", "info", "--json"});
    expect(info.success && info.command.kind == command_kind::info && info.command.info_json,
           "info --json should parse");
    expect(!parse({"pm", "info", "extra"}).success, "info extra argument should fail");

    auto graph = parse({"pm", "graph", "--no-color", "api", "--json"});
    expect(graph.success && graph.command.kind == command_kind::graph, "graph command should parse");
    expect(graph.command.graph_options.focus == "api" && graph.command.graph_options.json &&
           graph.command.graph_options.no_color, "graph options should parse in any order");
    expect(parse({"pm", "dag", "worker", "--dot"}).command.kind == command_kind::graph,
           "dag alias should parse");
    expect(!parse({"pm", "graph", "--json", "--dot"}).success, "graph formats should conflict");

    auto restart = parse({"pm", "restart", "app", "--log"});
    expect(restart.success && restart.command.name == "app" && restart.command.show_log,
           "restart --log should parse");
    expect(!parse({"pm", "stop"}).success, "named command should require a name");

    auto start = parse({"pm", "start", "api", "--kill-timeout", "5",
                        "--depends-on", "db", "--depends-on", "cache", "--failure-action", "restart",
                        "--heartbeat-timeout", "9", "--env", "A=B", "--no-pty", "--log",
                        "--log-mode", "combined", "--log-dir", "var/log", "--log-file-name", "api.log",
                        "--log-max-size-kb", "8192", "--log-archive-count", "5", "--",
                        "./server", "--flag", "value with space", "", "-x"});
    expect(start.success && start.command.kind == command_kind::start, "start should parse");
    expect(start.command.start.name == "api" && start.command.start.kill_timeout_sec == 5,
           "start scalar options should parse");
    expect(start.command.start.depends_on == std::vector<std::string>({"db", "cache"}),
           "repeated dependencies should parse");
    expect(start.command.start.failure_action == pm_tiny::failure_action_t::RESTART,
           "failure action should parse");
    expect(!start.command.start.pty && start.command.start.show_log, "start flags should parse");
    expect(start.command.start.log_mode == pm_tiny::log_mode_t::combined &&
           start.command.start.log_dir == "var/log" && start.command.start.log_file_name == "api.log" &&
           start.command.start.log_max_size_kb == 8192 && start.command.start.log_archive_count == 5,
           "log definition options should parse");
    expect(start.command.start.executable == "./server" &&
           start.command.start.args == std::vector<std::string>({"--flag", "value with space", "", "-x"}),
           "argv after -- should be preserved exactly");
    expect(parse({"pm", "start", "configured", "--log"}).success,
           "configured start with log should parse");
    expect(!parse({"pm", "start", "app", "--kill-timeout", "invalid", "--", "/bin/true"}).success,
           "invalid integer should fail");
    expect(!parse({"pm", "start", "app", "--unknown"}).success, "unknown start option should fail");
    expect(!parse({"pm", "start", "app", "--name", "old", "--", "/bin/true"}).success,
           "removed --name should fail");
    expect(!parse({"pm", "start", "app", "--no_daemon", "--", "/bin/true"}).success,
           "underscore options should fail");
    expect(!parse({"pm", "start", "app", "--env", "PM_TINY_HOME=x", "--", "/bin/true"}).success,
           "reserved environment overrides should fail");
    expect(!parse({"pm", "start"}).success, "start should require a name");
    expect(!parse({"pm", "start", "configured", "--log-mode", "split"}).success,
           "log definition option without executable should fail");
    expect(!parse({"pm", "start", "app", "--log-mode", "bad", "--", "/bin/true"}).success,
           "invalid log mode should fail");
    expect(!parse({"pm", "start", "app", "--log-archive-count", "101", "--", "/bin/true"}).success,
           "invalid log archive count should fail");

    expect(pm_tiny::cli::command_protocol_type(command_kind::restart) == 0x28,
           "restart protocol mapping should be shared");
    expect(pm_tiny::cli::command_protocol_type(command_kind::info) == 0x36,
           "info protocol mapping should be shared");
    expect(pm_tiny::cli::command_usage("pm", true).find("info [--json]") != std::string::npos,
           "info help text missing");
    expect(parse({"pm", "shutdown"}).command.kind == command_kind::quit, "shutdown alias should parse");
    return 0;
}
