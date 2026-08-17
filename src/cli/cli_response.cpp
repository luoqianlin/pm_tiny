#include "cli_response.h"

#include "daemon_info.h"
#include "daemon_info_renderer.h"
#include "dependency_graph_renderer.h"
#include "inspect_renderer.h"
#include "process_list.h"
#include "process_list_renderer.h"
#include "protocol_v3.h"
#include "runtime_snapshot.h"

namespace pm_tiny {
namespace cli {
namespace {

std::string trim_message(std::string message) {
    if (message.compare(0, 4, "ERR ") == 0) message.erase(0, 4);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
    return message;
}

std::string error_text(std::int32_t status, const std::string &message) {
    const auto clean = trim_message(message);
    if (clean.compare(0, 10, "pm: error(") == 0) return clean + "\n";
    return "pm: error(" + std::to_string(status) + "): " + clean + "\n";
}

bool action_is_silent(command_kind kind) {
    return kind == command_kind::stop || kind == command_kind::restart ||
           kind == command_kind::remove || kind == command_kind::save ||
           kind == command_kind::reload || kind == command_kind::quit;
}

bool should_post_list(const parsed_command &command) {
    if (command.no_list || command.show_log) return false;
    return command.kind == command_kind::stop || command.kind == command_kind::restart ||
           command.kind == command_kind::remove || command.kind == command_kind::reload;
}

} // namespace

cli_response_result interpret_control_response(
    const parsed_command &command,
    const frame_t &payload,
    const std::string &client_version,
    const list_render_options *list_options,
    const dependency_graph_render_options *graph_options) {
    iframe_stream stream(payload);
    std::int32_t status = -1;
    std::string message;
    stream >> status >> message;

    cli_response_result result;
    if (status != 0) {
        result.stderr_text = error_text(status, message);
        return result;
    }
    result.success = true;
    result.post_list = should_post_list(command);

    if (command.kind == command_kind::list || command.kind == command_kind::graph) {
        const auto entries = read_process_list(stream);
        if (command.kind == command_kind::graph) {
            if (graph_options == nullptr) throw protocol_error("missing graph render options");
            result.stdout_text = render_dependency_graph(entries, *graph_options);
        } else {
            if (list_options == nullptr) throw protocol_error("missing list render options");
            result.stdout_text = render_process_list(entries, *list_options);
        }
    } else if (command.kind == command_kind::inspect) {
        result.stdout_text = render_inspect_snapshot(read_inspect_snapshot(stream));
    } else if (command.kind == command_kind::info) {
        result.stdout_text = render_daemon_info(read_daemon_info(stream), command.info_json);
    } else if (command.kind == command_kind::version) {
        std::string daemon_version;
        stream >> daemon_version;
        result.stdout_text = "pm: " + client_version + "\npm_tiny: " + daemon_version + "\n";
    } else if (command.kind == command_kind::quit) {
        std::int32_t pid = -1;
        stream >> pid;
        result.daemon_pid = pid;
    } else if (command.kind == command_kind::start) {
        const auto start = read_start_response(stream.remaining_frame());
        if (start.result == start_result::blocked) {
            result.success = false;
            std::string blocked;
            for (std::size_t i = 0; i < start.blocked_by.size(); ++i) {
                if (i != 0) blocked += ",";
                blocked += start.blocked_by[i];
            }
            result.stderr_text = "pm: error(-7): blocked `" + command.start.name + "` by: " + blocked + "\n";
        } else if (command.start.show_log) {
            result.stream_expected = true;
        } else if (start.result == start_result::started) {
            result.stdout_text = "started `" + command.start.name + "` pid=" + std::to_string(start.pid);
            if (command.start.create) result.stdout_text += "; run `pm save` to persist";
            result.stdout_text += "\n";
        } else {
            result.stdout_text = "waiting `" + command.start.name + "`";
            if (!start.blocked_by.empty()) {
                result.stdout_text += " for: ";
                for (std::size_t i = 0; i < start.blocked_by.size(); ++i) {
                    if (i != 0) result.stdout_text += ",";
                    result.stdout_text += start.blocked_by[i];
                }
            }
            result.stdout_text += "\n";
        }
    } else if (command.kind == command_kind::log ||
               (command.kind == command_kind::restart && command.show_log)) {
        result.log_response = read_program_log_response(stream);
        const auto expected_mode = command.log_history ? log_request_mode::history : log_request_mode::live;
        if (result.log_response.mode != expected_mode)
            throw protocol_error("program-log response mode mismatch");
        if (command.kind == command_kind::log && command.log_history)
            result.stdout_text = format_program_log_history_header(command.name, result.log_response);
        result.stream_expected = true;
    } else if (!action_is_silent(command.kind)) {
        result.stdout_text = trim_message(message);
        if (!result.stdout_text.empty()) result.stdout_text += "\n";
    }
    return result;
}

} // namespace cli
} // namespace pm_tiny
