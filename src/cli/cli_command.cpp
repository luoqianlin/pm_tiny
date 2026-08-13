#include "cli_command.h"

#include "pm_tiny.h"
#include "control_command.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace pm_tiny {
namespace cli {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_integer(const std::string &text, int &value) {
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(text, &consumed, 10);
        if (consumed != text.size() || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::vector<std::string> split_dependencies(const std::string &value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        auto item = value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        const auto first = item.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto last = item.find_last_not_of(" \t\r\n");
            result.push_back(item.substr(first, last - first + 1));
        }
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return result;
}

command_parse_result failure(std::string message) {
    command_parse_result result;
    result.error = std::move(message);
    return result;
}

command_parse_result parse_start(const std::vector<std::string> &args) {
    if (args.size() < 3) return failure("start requires a command or configured process name");

    command_parse_result result;
    result.success = true;
    result.command.kind = command_kind::start;
    result.command.start.command = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
        const auto &option = args[i];
        auto require_value = [&](std::string &target) -> bool {
            if (i + 1 >= args.size()) {
                result = failure(option + " requires a value");
                return false;
            }
            target = args[++i];
            result.command.start.has_definition_options = true;
            return true;
        };
        auto require_integer = [&](int &target) -> bool {
            std::string value;
            if (!require_value(value)) return false;
            if (!parse_integer(value, target)) {
                result = failure(option + " requires an integer, got `" + value + "`");
                return false;
            }
            return true;
        };

        if (option == "--name") {
            if (!require_value(result.command.start.name)) return result;
        } else if (option == "--kill_timeout") {
            if (!require_integer(result.command.start.kill_timeout_sec)) return result;
        } else if (option == "--user") {
            if (!require_value(result.command.start.run_as)) return result;
        } else if (option == "--env_var") {
            std::string value;
            if (!require_value(value)) return result;
            result.command.start.env_vars.push_back(std::move(value));
        } else if (option == "--depends_on") {
            std::string value;
            if (!require_value(value)) return result;
            result.command.start.depends_on = split_dependencies(value);
        } else if (option == "--start_timeout") {
            if (!require_integer(result.command.start.start_timeout)) return result;
        } else if (option == "--failure_action") {
            std::string value;
            if (!require_value(value)) return result;
            try {
                result.command.start.failure_action = str_to_failure_action(value);
            } catch (const std::exception &error) {
                return failure(error.what());
            }
        } else if (option == "--heartbeat_timeout") {
            if (!require_integer(result.command.start.heartbeat_timeout)) return result;
        } else if (option == "--oom_score_adj") {
            if (!require_integer(result.command.start.oom_score_adj)) return result;
        } else if (option == "--no_daemon") {
            result.command.start.daemon = false;
            result.command.start.has_definition_options = true;
        } else if (option == "--log") {
            result.command.start.show_log = true;
            result.command.start.has_definition_options = true;
        } else if (option == "--no_pty") {
            result.command.start.pty = false;
            result.command.start.has_definition_options = true;
        } else if (option == "--pty") {
            result.command.start.pty = true;
            result.command.start.has_definition_options = true;
        } else {
            return failure("unknown start option: " + option);
        }
    }
    return result;
}

} // namespace

command_parse_result parse_command_line(int argc, char *argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    return parse_command_line(args);
}

command_parse_result parse_command_line(const std::vector<std::string> &args) {
    if (args.size() < 2) return failure("a command is required");

    const auto name = lowercase(args[1]);
    command_parse_result result;
    result.success = true;
    if (name == "help" || name == "--help" || name == "-h") {
        result.command.kind = command_kind::help;
        if (args.size() != 2) return failure("help does not accept arguments");
        return result;
    }
    if (name == "version" || name == "--version" || name == "-v") {
        result.command.kind = command_kind::version;
        if (args.size() != 2) return failure("version does not accept arguments");
        return result;
    }
    if (name == "start") return parse_start(args);

    if (name == "list" || name == "ls" || name == "status") {
        result.command.kind = command_kind::list;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--wide") result.command.list_options.wide = true;
            else if (args[i] == "--json") result.command.list_options.json = true;
            else if (args[i] == "--no-color") result.command.list_options.no_color = true;
            else return failure("unexpected list argument: " + args[i]);
        }
        if (result.command.list_options.wide && result.command.list_options.json) {
            return failure("--wide and --json cannot be used together");
        }
        return result;
    }

    if (name == "graph" || name == "dag") {
        result.command.kind = command_kind::graph;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json") result.command.graph_options.json = true;
            else if (args[i] == "--dot") result.command.graph_options.dot = true;
            else if (args[i] == "--no-color") result.command.graph_options.no_color = true;
            else if (!args[i].empty() && args[i][0] == '-') {
                return failure("unexpected graph argument: " + args[i]);
            } else if (!result.command.graph_options.focus.empty()) {
                return failure("graph accepts at most one process name");
            } else {
                result.command.graph_options.focus = args[i];
            }
        }
        if (result.command.graph_options.json && result.command.graph_options.dot) {
            return failure("--json and --dot cannot be used together");
        }
        return result;
    }

    struct named_command { const char *name; command_kind kind; bool allow_log; };
    static constexpr named_command named_commands[] = {
        {"stop", command_kind::stop, false},
        {"restart", command_kind::restart, true},
        {"delete", command_kind::remove, false},
        {"log", command_kind::log, false},
        {"inspect", command_kind::inspect, false},
    };
    for (const auto &candidate : named_commands) {
        if (name != candidate.name) continue;
        result.command.kind = candidate.kind;
        if (args.size() < 3) return failure(name + " requires a process name");
        result.command.name = args[2];
        if (args.size() == 4 && candidate.allow_log && args[3] == "--log") {
            result.command.show_log = true;
        } else if (args.size() != 3) {
            return failure("unexpected " + name + " argument: " + args[3]);
        }
        return result;
    }

    struct plain_command { const char *name; command_kind kind; };
    static constexpr plain_command plain_commands[] = {
        {"save", command_kind::save},
        {"reload", command_kind::reload},
        {"quit", command_kind::quit},
        {"shutdown", command_kind::quit},
    };
    for (const auto &candidate : plain_commands) {
        if (name != candidate.name) continue;
        if (args.size() != 2) return failure(name + " does not accept arguments");
        result.command.kind = candidate.kind;
        return result;
    }
    return failure("unknown command: " + args[1]);
}

std::uint16_t command_protocol_type(command_kind kind) {
    switch (kind) {
        case command_kind::list: return static_cast<std::uint16_t>(control_command::list);
        case command_kind::graph: return static_cast<std::uint16_t>(control_command::list);
        case command_kind::stop: return static_cast<std::uint16_t>(control_command::stop);
        case command_kind::start: return static_cast<std::uint16_t>(control_command::start);
        case command_kind::save: return static_cast<std::uint16_t>(control_command::save);
        case command_kind::remove: return static_cast<std::uint16_t>(control_command::remove);
        case command_kind::restart: return static_cast<std::uint16_t>(control_command::restart);
        case command_kind::version: return static_cast<std::uint16_t>(control_command::version);
        case command_kind::log: return static_cast<std::uint16_t>(control_command::log);
        case command_kind::inspect: return static_cast<std::uint16_t>(control_command::inspect);
        case command_kind::reload: return static_cast<std::uint16_t>(control_command::reload);
        case command_kind::quit: return static_cast<std::uint16_t>(control_command::quit);
        case command_kind::help: break;
    }
    throw std::invalid_argument("command has no protocol type");
}

std::string command_usage(const std::string &executable, bool dynamic_start_supported) {
    std::ostringstream out;
    out << "Usage: " << executable << " <command> [options]\n\n"
        << "Commands:\n"
        << "  list|ls|status [--wide|--json] [--no-color]\n"
        << "  graph|dag [name] [--json|--dot] [--no-color]\n"
        << "  start " << (dynamic_start_supported ? "<command> [process options]" : "<configured-name>") << "\n"
        << "  stop <name>\n"
        << "  restart <name> [--log]\n"
        << "  delete <name>\n"
        << "  log <name>\n"
        << "  inspect <name>\n"
        << "  save\n"
        << "  reload\n"
        << "  quit\n"
        << "  version\n";
    if (dynamic_start_supported) {
        out << "\nProcess options:\n"
            << "  --name <name> --kill_timeout <seconds> --user <user>\n"
            << "  --env_var <key=value> --depends_on <app,...>\n"
            << "  --start_timeout <seconds> --failure_action <skip|restart|reboot>\n"
            << "  --heartbeat_timeout <seconds> --oom_score_adj <-1000..1000>\n"
            << "  --no_daemon --log --pty|--no_pty\n";
    }
    return out.str();
}

} // namespace cli
} // namespace pm_tiny
