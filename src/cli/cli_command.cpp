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

command_parse_result failure(std::string message) {
    command_parse_result result;
    result.error = std::move(message);
    return result;
}

command_parse_result parse_start(const std::vector<std::string> &args) {
    if (args.size() < 3 || args[2].empty() || args[2][0] == '-') return failure("start requires an explicit process name");

    command_parse_result result;
    result.success = true;
    result.command.kind = command_kind::start;
    result.command.start.name = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
        const auto &option = args[i];
        if (option == "--") {
            if (i + 1 >= args.size()) return failure("-- requires an executable");
            result.command.start.create = true;
            result.command.start.executable = args[++i];
            result.command.start.args.assign(args.begin() + static_cast<std::ptrdiff_t>(i + 1), args.end());
            return result;
        }
        auto require_value = [&](std::string &target) -> bool {
            if (i + 1 >= args.size()) {
                result = failure(option + " requires a value");
                return false;
            }
            target = args[++i];
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

        if (option == "--cwd") {
            if (!require_value(result.command.start.cwd)) return result;
        } else if (option == "--kill-timeout") {
            if (!require_integer(result.command.start.kill_timeout_sec)) return result;
        } else if (option == "--user") {
            if (!require_value(result.command.start.run_as)) return result;
        } else if (option == "--env") {
            std::string value;
            if (!require_value(value)) return result;
            if (value.find('=') == std::string::npos || value[0] == '=') return failure("--env requires KEY=VALUE");
            if (value.rfind("PM_TINY_", 0) == 0) return failure("--env cannot override reserved PM_TINY_* variables");
            result.command.start.env.push_back(std::move(value));
        } else if (option == "--depends-on") {
            std::string value;
            if (!require_value(value)) return result;
            result.command.start.depends_on.push_back(std::move(value));
        } else if (option == "--start-timeout") {
            if (!require_integer(result.command.start.start_timeout)) return result;
        } else if (option == "--failure-action") {
            std::string value;
            if (!require_value(value)) return result;
            try {
                result.command.start.failure_action = str_to_failure_action(value);
            } catch (const std::exception &error) {
                return failure(error.what());
            }
        } else if (option == "--heartbeat-timeout") {
            if (!require_integer(result.command.start.heartbeat_timeout)) return result;
        } else if (option == "--oom-score-adj") {
            if (!require_integer(result.command.start.oom_score_adj)) return result;
        } else if (option == "--daemon") {
            result.command.start.daemon = true;
        } else if (option == "--no-daemon") {
            result.command.start.daemon = false;
        } else if (option == "--log") {
            result.command.start.show_log = true;
        } else if (option == "--log-mode") {
            std::string value;
            if (!require_value(value)) return result;
            if (!parse_log_mode(value, result.command.start.log_mode))
                return failure("--log-mode requires `split` or `combined`");
            result.command.start.log_mode_explicit = true;
        } else if (option == "--log-dir") {
            if (!require_value(result.command.start.log_dir)) return result;
        } else if (option == "--log-file-name") {
            if (!require_value(result.command.start.log_file_name)) return result;
        } else if (option == "--log-max-size-kb") {
            if (!require_integer(result.command.start.log_max_size_kb)) return result;
            if (result.command.start.log_max_size_kb < 1 || result.command.start.log_max_size_kb > 1048576)
                return failure("--log-max-size-kb must be between 1 and 1048576");
        } else if (option == "--log-archive-count") {
            if (!require_integer(result.command.start.log_archive_count)) return result;
            if (result.command.start.log_archive_count < 0 || result.command.start.log_archive_count > 100)
                return failure("--log-archive-count must be between 0 and 100");
        } else if (option == "--no-pty") {
            result.command.start.pty = false;
        } else if (option == "--pty") {
            result.command.start.pty = true;
        } else if (option == "--restart-delay-ms") {
            if (!require_integer(result.command.start.restart_delay_ms)) return result;
        } else if (option == "--restart-max-delay-ms") {
            if (!require_integer(result.command.start.restart_max_delay_ms)) return result;
        } else if (option == "--restart-window-ms") {
            if (!require_integer(result.command.start.restart_window_ms)) return result;
        } else if (option == "--restart-max-attempts") {
            if (!require_integer(result.command.start.restart_max_attempts)) return result;
        } else if (option == "--restart-reset-after-ms") {
            if (!require_integer(result.command.start.restart_reset_after_ms)) return result;
        } else {
            return failure("unknown start option: " + option);
        }
    }
    if (!result.command.start.create) {
        const auto &s = result.command.start;
        if (!s.cwd.empty() || !s.run_as.empty() || !s.env.empty() || !s.depends_on.empty() || s.pty ||
            !s.daemon || s.kill_timeout_sec != 3 || s.start_timeout != 0 ||
            s.failure_action != failure_action_t::SKIP || s.heartbeat_timeout != -1 || s.oom_score_adj != 0 ||
            s.restart_delay_ms != 1000 || s.restart_max_delay_ms != 30000 || s.restart_window_ms != 60000 ||
            s.restart_max_attempts != 10 || s.restart_reset_after_ms != 60000 || s.log_mode_explicit ||
            !s.log_dir.empty() || !s.log_file_name.empty() || s.log_max_size_kb != 4096 ||
            s.log_archive_count != 3) {
            return failure("definition options require `-- <executable> [args...]`");
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

    if (name == "info") {
        result.command.kind = command_kind::info;
        if (args.size() == 3 && args[2] == "--json") result.command.info_json = true;
        else if (args.size() != 2) return failure("unexpected info argument: " + args[2]);
        return result;
    }

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

    struct named_command { const char *name; command_kind kind; bool allow_log; bool allow_no_list; bool allow_history; };
    static constexpr named_command named_commands[] = {
        {"stop", command_kind::stop, false, true, false},
        {"restart", command_kind::restart, true, true, false},
        {"delete", command_kind::remove, false, true, false},
        {"log", command_kind::log, false, false, true},
        {"inspect", command_kind::inspect, false, false, false},
    };
    for (const auto &candidate : named_commands) {
        if (name != candidate.name) continue;
        result.command.kind = candidate.kind;
        if (args.size() < 3) return failure(name + " requires a process name");
        result.command.name = args[2];
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (candidate.allow_log && args[i] == "--log") {
                result.command.show_log = true;
            } else if (candidate.allow_no_list && args[i] == "--no-list") {
                result.command.no_list = true;
            } else if (candidate.allow_history && args[i] == "--history") {
                result.command.log_history = true;
            } else {
                return failure("unexpected " + name + " argument: " + args[i]);
            }
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
        if (name == "reload" && args.size() == 3 && args[2] == "--no-list") {
            result.command.no_list = true;
        } else if (args.size() != 2) {
            return failure(name + " does not accept arguments");
        }
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
        case command_kind::info: return static_cast<std::uint16_t>(control_command::info);
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
        << "  start <name> [--log]\n"
        << "  start <name> [options] -- <executable> [args...]\n"
        << "  restart <name> [--log] [--no-list]\n"
        << "  stop <name> [--no-list]\n"
        << "  delete <name> [--no-list]\n"
        << "  log <name> [--history]\n"
        << "  inspect <name>\n"
        << "  info [--json]\n"
        << "  save\n"
        << "  reload [--no-list]\n"
        << "  quit\n"
        << "  version\n";
    out << "\nControl options:\n"
        << "  --no-list                 suppress the post-command process list\n";
    if (dynamic_start_supported) {
        out << "\nProcess options:\n"
            << "  --cwd <path> --kill-timeout <seconds> --user <user>\n"
            << "  --env <key=value> --depends-on <name>\n"
            << "  --start-timeout <seconds> --failure-action <skip|restart|reboot>\n"
            << "  --heartbeat-timeout <seconds> --oom-score-adj <-1000..1000>\n"
            << "  --daemon|--no-daemon --log --pty|--no-pty\n"
            << "  --log-mode split|combined --log-dir <dir> --log-file-name <name>\n"
            << "  --log-max-size-kb <n> --log-archive-count <n>\n"
            << "  --restart-delay-ms --restart-max-delay-ms --restart-window-ms\n"
            << "  --restart-max-attempts --restart-reset-after-ms\n";
    }
    return out.str();
}

} // namespace cli
} // namespace pm_tiny
