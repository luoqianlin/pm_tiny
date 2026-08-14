#include <windows.h>
#include "protocol_v3.h"
#include "frame_stream.hpp"
#include "win_utils.h"
#include "process_list.h"
#include "process_list_renderer.h"
#include "inspect_renderer.h"
#include "runtime_snapshot.h"
#include "dependency_graph_renderer.h"
#include "connection_error.h"
#include "cli_command.h"
#include "daemon_config.h"
#include "daemon_info.h"
#include "daemon_info_renderer.h"
#include "pm_tiny.h"

#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace {

std::atomic_bool g_interrupted{false};
HANDLE g_active_pipe = INVALID_HANDLE_VALUE;

BOOL WINAPI cli_ctrl_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    g_interrupted.store(true);
    if (g_active_pipe != INVALID_HANDLE_VALUE) CancelIoEx(g_active_pipe, nullptr);
    return TRUE;
}

std::string windows_error_message(DWORD error) {
    wchar_t *buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error, 0,
                                        reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) return "Windows error";
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ' || message.back() == L'.')) {
        message.pop_back();
    }
    try {
        return pm_tiny::win::wide_to_utf8(message);
    } catch (...) {
        return "Windows error";
    }
}

HANDLE connect_to_pipe() {
    constexpr DWORD kWaitTimeoutMs = 100;
    constexpr int kMaxAttempts = 20;

    const auto name = pm_tiny::win::control_pipe_name_wide();
    DWORD last_error = ERROR_FILE_NOT_FOUND;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        HANDLE pipe = CreateFileW(name.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  0,
                                  nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        last_error = GetLastError();
        if (last_error == ERROR_FILE_NOT_FOUND) {
            Sleep(kWaitTimeoutMs);
            continue;
        }
        if (last_error != ERROR_PIPE_BUSY) {
            break;
        }
        if (!WaitNamedPipeW(name.c_str(), kWaitTimeoutMs)) {
            last_error = GetLastError();
        }
    }
    SetLastError(last_error);
    return INVALID_HANDLE_VALUE;
}

bool send_command(const pm_tiny::cli::parsed_command &command, std::string &response,
                  const pm_tiny::cli::list_render_options *list_options,
                  const pm_tiny::cli::dependency_graph_render_options *graph_options,
                  bool &command_success) {
    command_success = false;
    HANDLE pipe = connect_to_pipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        pm_tiny::cli::connection_error_info error_info;
        error_info.endpoint = pm_tiny::win::control_pipe_name();
        error_info.transport = pm_tiny::cli::connection_transport::windows_named_pipe;
        error_info.reason = windows_error_message(error);
        error_info.code_name = "winerror";
        error_info.code = error;
        error_info.category = pm_tiny::cli::classify_windows_connection_error(error);
        std::cerr << pm_tiny::cli::format_connection_error(error_info);
        return false;
    }
    g_active_pipe = pipe;

    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        DWORD error = GetLastError();
        std::cerr << "Failed to configure control pipe (error " << error << ")." << std::endl;
        g_active_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        return false;
    }

    const auto type = pm_tiny::cli::command_protocol_type(command.kind);
    pm_tiny::protocol_message request;
    request.type = type;
    request.request_id = 1;
    if (command.kind == pm_tiny::cli::command_kind::start) {
        pm_tiny::start_request start;
        start.mode = command.start.create ? pm_tiny::start_mode::create : pm_tiny::start_mode::existing;
        start.name = command.start.name;
        start.show_log = command.start.show_log;
        if (command.start.create) {
            start.config.name = command.start.name;
            start.config.cwd = command.start.cwd.empty() ? "." : command.start.cwd;
            start.config.executable = command.start.executable;
            start.config.args = command.start.args;
            start.config.kill_timeout_s = command.start.kill_timeout_sec;
            start.config.run_as = command.start.run_as;
            start.config.env_vars = command.start.env;
            start.config.depends_on = command.start.depends_on;
            start.config.start_timeout = command.start.start_timeout;
            start.config.failure_action = command.start.failure_action;
            start.config.daemon = command.start.daemon;
            start.config.heartbeat_timeout = command.start.heartbeat_timeout;
            start.config.oom_score_adj = command.start.oom_score_adj;
            start.config.pty = command.start.pty;
            start.config.restart_delay_ms = command.start.restart_delay_ms;
            start.config.restart_max_delay_ms = command.start.restart_max_delay_ms;
            start.config.restart_window_ms = command.start.restart_window_ms;
            start.config.restart_max_attempts = command.start.restart_max_attempts;
            start.config.restart_reset_after_ms = command.start.restart_reset_after_ms;
            LPWCH environment = GetEnvironmentStringsW();
            if (environment != nullptr) {
                for (LPCWCH item = environment; *item != L'\0'; item += wcslen(item) + 1) {
                    try {
                        const auto value = pm_tiny::win::wide_to_utf8(item);
                        if (value.rfind("PM_TINY_", 0) != 0 && !value.empty() && value[0] != '=')
                            start.inherited_env.push_back(value);
                    } catch (...) {}
                }
                FreeEnvironmentStringsW(environment);
            }
        }
        pm_tiny::append_start_request(request.payload, start);
    } else if (!command.name.empty()) {
        pm_tiny::fappend_value(request.payload, command.name);
    }
    auto payload = pm_tiny::protocol_encode(request);

    DWORD bytes_written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr)) {
        DWORD error = GetLastError();
        std::cerr << "Failed to send command (error " << error << ")." << std::endl;
        g_active_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        return false;
    }
    FlushFileBuffers(pipe);

    std::int32_t command_status = -1;
    try {
        pm_tiny::protocol_decoder decoder;
        bool received_response = false;
        bool received_stream = false;
        bool stream_finished = false;
        constexpr DWORD chunk_size = 16 * 1024;
        std::vector<std::uint8_t> buffer(chunk_size);
        while (!stream_finished) {
            DWORD bytes_read = 0;
            const BOOL read_ok = ReadFile(pipe, buffer.data(), chunk_size, &bytes_read, nullptr);
            if (!read_ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_OPERATION_ABORTED && g_interrupted.load()) break;
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) break;
                throw std::runtime_error("named pipe read failed: " + std::to_string(error));
            }
            if (bytes_read == 0) continue;
            decoder.feed(buffer.data(), bytes_read);
            while (!decoder.empty()) {
                auto message = decoder.pop();
                if (message.flags & pm_tiny::protocol_flag_stream) {
                    if (!received_response || stream_finished)
                        throw pm_tiny::protocol_error("unexpected stream frame");
                    pm_tiny::iframe_stream stream_payload(message.payload);
                    std::int32_t stream_type = 0;
                    std::string chunk;
                    stream_payload >> stream_type >> chunk;
                    if (!received_stream) {
                        if (!response.empty()) {
                            std::cout << response;
                            if (response.back() != '\n') std::cout << std::endl;
                            response.clear();
                        }
                        received_stream = true;
                    }
                    if (!chunk.empty()) {
                        std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                        std::cout.flush();
                    }
                    stream_finished = !(message.flags & pm_tiny::protocol_flag_more);
                    continue;
                }
                if (received_response) throw pm_tiny::protocol_error("unexpected extra response frame");
                received_response = true;
                pm_tiny::iframe_stream stream(message.payload);
                std::int32_t status = -1;
                std::string text;
                stream >> status >> text;
                if (type == 0x23 && status == 0) {
                    const auto entries = pm_tiny::read_process_list(stream);
                    if (command.kind == pm_tiny::cli::command_kind::graph) {
                        if (graph_options == nullptr) throw pm_tiny::protocol_error("missing graph render options");
                        response = pm_tiny::cli::render_dependency_graph(entries, *graph_options);
                    } else {
                        if (list_options == nullptr) throw pm_tiny::protocol_error("missing list render options");
                        response = pm_tiny::cli::render_process_list(entries, *list_options);
                    }
                } else if (command.kind == pm_tiny::cli::command_kind::inspect && status == 0) {
                    response = pm_tiny::cli::render_inspect_snapshot(pm_tiny::read_inspect_snapshot(stream));
                } else if (command.kind == pm_tiny::cli::command_kind::info && status == 0) {
                    response = pm_tiny::cli::render_daemon_info(pm_tiny::read_daemon_info(stream), command.info_json);
                } else if (command.kind == pm_tiny::cli::command_kind::start && status == 0) {
                    const auto start = pm_tiny::read_start_response(stream.remaining_frame());
                    if (start.result == pm_tiny::start_result::started) {
                        response = "started `" + command.start.name + "` pid=" + std::to_string(start.pid);
                        if (command.start.create) response += "; run `pm save` to persist";
                    } else if (start.result == pm_tiny::start_result::waiting) {
                        response = "waiting `" + command.start.name + "`";
                        if (!start.blocked_by.empty()) {
                            response += " for: ";
                            for (std::size_t i = 0; i < start.blocked_by.size(); ++i) {
                                if (i != 0) response += ",";
                                response += start.blocked_by[i];
                            }
                        }
                    } else {
                        response = "blocked `" + command.start.name + "`";
                        status = -1;
                    }
                } else {
                    response = text;
                }
                if (status != 0 && response.rfind("ERR", 0) != 0) response = "ERR " + response;
                command_status = status;
                if (!(message.flags & pm_tiny::protocol_flag_more) &&
                    command.kind != pm_tiny::cli::command_kind::log &&
                    !(command.kind == pm_tiny::cli::command_kind::start && command.start.show_log) &&
                    !(command.kind == pm_tiny::cli::command_kind::restart && command.show_log)) {
                    stream_finished = true;
                }
            }
        }
        if (!received_response) {
            if (g_interrupted.load()) throw std::runtime_error("interrupted");
            throw pm_tiny::protocol_error("response ended before first frame");
        }
        if (received_stream && !stream_finished)
            throw pm_tiny::protocol_error("stream ended before final chunk");
    } catch (const std::exception &ex) {
        if (!g_interrupted.load()) std::cerr << "Invalid response from pm_tiny: " << ex.what() << std::endl;
        g_active_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        return false;
    }
    g_active_pipe = INVALID_HANDLE_VALUE;
    CloseHandle(pipe);
    command_success = command_status == 0;
    return !g_interrupted.load();
}

} // namespace

int wmain(int argc, wchar_t *wide_argv[]) {
    std::vector<std::string> utf8_arguments;
    std::vector<char *> argv;
    utf8_arguments.reserve(static_cast<std::size_t>(argc));
    argv.reserve(static_cast<std::size_t>(argc));
    try {
        for (int i = 0; i < argc; ++i) utf8_arguments.push_back(pm_tiny::win::wide_to_utf8(wide_argv[i]));
    } catch (const std::exception &ex) {
        std::cerr << "pm: invalid command line: " << ex.what() << std::endl;
        return 2;
    }
    for (auto &argument : utf8_arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    SetConsoleCtrlHandler(cli_ctrl_handler, TRUE);
    const auto parsed = pm_tiny::cli::parse_command_line(argc, argv.data());
    if (!parsed.success) {
        std::cerr << "pm: " << parsed.error << "\n\n"
                  << pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", false);
        return 2;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::help) {
        std::cout << pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", false);
        return EXIT_SUCCESS;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::start && parsed.command.start.pty) {
        std::cerr << "pm: --pty is unsupported on Windows" << std::endl;
        return EXIT_FAILURE;
    }
    const pm_tiny::daemon_cli_options daemon_options;
    const auto daemon_config = pm_tiny::resolve_daemon_config(
        daemon_options, pm_tiny::daemon_platform::windows);
    if (!daemon_config.success) {
        std::cerr << "pm: " << daemon_config.error << std::endl;
        return EXIT_FAILURE;
    }
    std::string environment_error;
    if (!pm_tiny::set_daemon_environment(PM_TINY_PIPE_NAME,
                                         daemon_config.config.pipe_name,
                                         environment_error)) {
        std::cerr << "pm: " << environment_error << std::endl;
        return EXIT_FAILURE;
    }

    auto list_options = parsed.command.list_options;
    const bool is_list = parsed.command.kind == pm_tiny::cli::command_kind::list;
    const bool is_graph = parsed.command.kind == pm_tiny::cli::command_kind::graph;
    if (is_list) {
        list_options.terminal_width = pm_tiny::cli::stdout_terminal_width();
        list_options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
    }
    auto graph_options = parsed.command.graph_options;
    if (is_graph) graph_options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
    std::string response;
    bool command_success = false;
    if (!send_command(parsed.command, response,
                      is_list ? &list_options : nullptr,
                      is_graph ? &graph_options : nullptr,
                      command_success)) {
        return g_interrupted.load() ? 130 : EXIT_FAILURE;
    }
    if (!response.empty()) {
        std::cout << response;
        if (response.back() != '\n') {
            std::cout << std::endl;
        }
    }
    return command_success ? EXIT_SUCCESS : EXIT_FAILURE;
}
