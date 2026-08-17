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
#include "cli_response.h"
#include "daemon_config.h"
#include "daemon_info.h"
#include "daemon_info_renderer.h"
#include "pm_tiny.h"

#include <iostream>
#include <cstdint>
#include <stdexcept>
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

bool has_path_separator(const std::wstring &value) {
    return value.find(L'\\') != std::wstring::npos || value.find(L'/') != std::wstring::npos;
}

bool has_extension(const std::wstring &value) {
    const auto separator = value.find_last_of(L"\\/");
    const auto dot = value.find_last_of(L'.');
    return dot != std::wstring::npos && (separator == std::wstring::npos || dot > separator);
}

std::wstring absolute_path(const std::wstring &value) {
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0) throw std::runtime_error("GetFullPathNameW failed");
    if (length >= buffer.size()) {
        buffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0 || length >= buffer.size()) throw std::runtime_error("GetFullPathNameW failed");
    }
    return std::wstring(buffer.data(), length);
}

bool regular_file(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring resolve_start_executable(const std::string &input, const std::wstring &cwd) {
    if (input.empty()) throw std::runtime_error("executable is empty");
    const auto requested = pm_tiny::win::utf8_to_wide(input);
    if (has_path_separator(requested)) {
        const bool rooted = requested[0] == L'\\' ||
            (requested.size() > 1 && requested[1] == L':');
        const auto path = absolute_path(rooted ? requested : cwd + L"\\" + requested);
        if (regular_file(path)) return path;
        if (!has_extension(path) && regular_file(path + L".exe")) return path + L".exe";
        throw std::runtime_error("resolve: executable not found: " + input);
    }

    const auto cwd_candidate = absolute_path(cwd + L"\\" + requested);
    if (regular_file(cwd_candidate)) return cwd_candidate;
    if (!has_extension(cwd_candidate) && regular_file(cwd_candidate + L".exe"))
        return cwd_candidate + L".exe";

    std::vector<wchar_t> buffer(32768);
    const DWORD length = SearchPathW(nullptr, requested.c_str(),
                                     has_extension(requested) ? nullptr : L".exe",
                                     static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
        throw std::runtime_error("resolve: executable not found: " + input);
    return absolute_path(std::wstring(buffer.data(), length));
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

bool send_command(const pm_tiny::cli::parsed_command &command,
                  const pm_tiny::cli::list_render_options *list_options,
                  const pm_tiny::cli::dependency_graph_render_options *graph_options,
                  pm_tiny::cli::cli_response_result &result) {
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
        pm_tiny::win::write_stderr_utf8(pm_tiny::cli::format_connection_error(error_info));
        return false;
    }
    g_active_pipe = pipe;

    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        DWORD error = GetLastError();
        pm_tiny::win::write_stderr_utf8("Failed to configure control pipe (error " +
                                        std::to_string(error) + ").\n");
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
            try {
                const std::wstring requested_cwd = command.start.cwd.empty()
                    ? std::wstring(L".") : pm_tiny::win::utf8_to_wide(command.start.cwd);
                std::vector<wchar_t> absolute_cwd(32768);
                DWORD cwd_length = GetFullPathNameW(requested_cwd.c_str(),
                                                    static_cast<DWORD>(absolute_cwd.size()),
                                                    absolute_cwd.data(), nullptr);
                if (cwd_length == 0) throw std::runtime_error("GetFullPathNameW failed");
                if (cwd_length >= absolute_cwd.size()) {
                    absolute_cwd.resize(static_cast<std::size_t>(cwd_length) + 1);
                    cwd_length = GetFullPathNameW(requested_cwd.c_str(),
                                                  static_cast<DWORD>(absolute_cwd.size()),
                                                  absolute_cwd.data(), nullptr);
                    if (cwd_length == 0 || cwd_length >= absolute_cwd.size())
                        throw std::runtime_error("GetFullPathNameW failed");
                }
                const std::wstring cwd_path(absolute_cwd.data(), cwd_length);
                const DWORD attributes = GetFileAttributesW(cwd_path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                    throw std::runtime_error("directory not found: " + command.start.cwd);
                if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
                    throw std::runtime_error("not a directory: " + command.start.cwd);
                start.config.cwd = pm_tiny::win::wide_to_utf8(cwd_path);
            } catch (const std::exception &error) {
                pm_tiny::win::write_stderr_utf8("pm: invalid --cwd: " + std::string(error.what()) + "\n");
                g_active_pipe = INVALID_HANDLE_VALUE;
                CloseHandle(pipe);
                return false;
            }
            try {
                start.config.executable = pm_tiny::win::wide_to_utf8(resolve_start_executable(
                    command.start.executable, pm_tiny::win::utf8_to_wide(start.config.cwd)));
            } catch (const std::exception &error) {
                pm_tiny::win::write_stderr_utf8("pm: " + std::string(error.what()) + "\n");
                g_active_pipe = INVALID_HANDLE_VALUE;
                CloseHandle(pipe);
                return false;
            }
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
            start.config.log_mode = command.start.log_mode;
            start.config.log_dir = command.start.log_dir;
            start.config.log_file_name = command.start.log_file_name;
            start.config.log_max_size_kb = command.start.log_max_size_kb;
            start.config.log_archive_count = command.start.log_archive_count;
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
    } else if (command.kind == pm_tiny::cli::command_kind::restart) {
        pm_tiny::fappend_value(request.payload, command.name);
        pm_tiny::fappend_value(request.payload, command.show_log ? 1 : 0);
    } else if (command.kind == pm_tiny::cli::command_kind::log) {
        pm_tiny::program_log_request log_request;
        log_request.name = command.name;
        log_request.mode = command.log_history ? pm_tiny::log_request_mode::history
                                               : pm_tiny::log_request_mode::live;
        pm_tiny::append_program_log_request(request.payload, log_request);
    } else if (!command.name.empty()) {
        pm_tiny::fappend_value(request.payload, command.name);
    }
    auto payload = pm_tiny::protocol_encode(request);

    DWORD bytes_written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr)) {
        DWORD error = GetLastError();
        pm_tiny::win::write_stderr_utf8("Failed to send command (error " +
                                        std::to_string(error) + ").\n");
        g_active_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        return false;
    }
    FlushFileBuffers(pipe);

    try {
        pm_tiny::protocol_decoder decoder;
        bool received_response = false;
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
                    if (!result.stream_expected)
                        throw pm_tiny::protocol_error("unexpected stream for command");
                    pm_tiny::iframe_stream stream_payload(message.payload);
                    std::int32_t stream_type = 0;
                    std::string chunk;
                    stream_payload >> stream_type >> chunk;
                    if (stream_type == 0) {
                        pm_tiny::win::write_stdout_utf8(chunk);
                    } else if (stream_type == 1) {
                        if (!chunk.empty()) {
                            std::cout.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                            std::cout.flush();
                        }
                    } else {
                        throw pm_tiny::protocol_error("unknown log stream type");
                    }
                    stream_finished = !(message.flags & pm_tiny::protocol_flag_more);
                    continue;
                }
                if (received_response) throw pm_tiny::protocol_error("unexpected extra response frame");
                received_response = true;
                result = pm_tiny::cli::interpret_control_response(
                    command, message.payload, PM_TINY_VERSION, list_options, graph_options);
                if (result.stream_expected && !result.stdout_text.empty()) {
                    pm_tiny::win::write_stdout_utf8(result.stdout_text);
                    result.stdout_text.clear();
                }
                if (!(message.flags & pm_tiny::protocol_flag_more) && !result.stream_expected) {
                    stream_finished = true;
                }
            }
        }
        if (!received_response) {
            if (g_interrupted.load()) throw std::runtime_error("interrupted");
            throw pm_tiny::protocol_error("response ended before first frame");
        }
        if (result.stream_expected && !stream_finished)
            throw pm_tiny::protocol_error("stream ended before final chunk");
    } catch (const std::exception &ex) {
        if (!g_interrupted.load())
            pm_tiny::win::write_stderr_utf8("Invalid response from pm_tiny: " + std::string(ex.what()) + "\n");
        g_active_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        return false;
    }
    g_active_pipe = INVALID_HANDLE_VALUE;
    CloseHandle(pipe);
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
        pm_tiny::win::write_stderr_utf8("pm: invalid command line: " + std::string(ex.what()) + "\n");
        return 2;
    }
    for (auto &argument : utf8_arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    SetConsoleCtrlHandler(cli_ctrl_handler, TRUE);
    const auto parsed = pm_tiny::cli::parse_command_line(argc, argv.data());
    if (!parsed.success) {
        pm_tiny::win::write_stderr_utf8("pm: " + parsed.error + "\n\n" +
            pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", true));
        return 2;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::help) {
        pm_tiny::win::write_stdout_utf8(pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", true));
        return EXIT_SUCCESS;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::start && parsed.command.start.pty) {
        pm_tiny::win::write_stderr_utf8("pm: --pty is unsupported on Windows\n");
        return EXIT_FAILURE;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::start && parsed.command.start.create &&
        (!parsed.command.start.run_as.empty() || parsed.command.start.oom_score_adj != 0 ||
         parsed.command.start.failure_action == pm_tiny::failure_action_t::REBOOT)) {
        pm_tiny::win::write_stderr_utf8("pm: unsupported Windows start option\n");
        return EXIT_FAILURE;
    }
    const pm_tiny::daemon_cli_options daemon_options;
    const auto daemon_config = pm_tiny::resolve_daemon_config(
        daemon_options, pm_tiny::daemon_platform::windows);
    if (!daemon_config.success) {
        pm_tiny::win::write_stderr_utf8("pm: " + daemon_config.error + "\n");
        return EXIT_FAILURE;
    }
    std::string environment_error;
    if (!pm_tiny::set_daemon_environment(PM_TINY_PIPE_NAME,
                                         daemon_config.config.pipe_name,
                                         environment_error)) {
        pm_tiny::win::write_stderr_utf8("pm: " + environment_error + "\n");
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
    pm_tiny::cli::cli_response_result result;
    if (!send_command(parsed.command,
                      is_list ? &list_options : nullptr,
                      is_graph ? &graph_options : nullptr,
                      result)) {
        return g_interrupted.load() ? 130 : EXIT_FAILURE;
    }
    if (!result.stderr_text.empty()) pm_tiny::win::write_stderr_utf8(result.stderr_text);
    if (!result.stdout_text.empty()) pm_tiny::win::write_stdout_utf8(result.stdout_text);
    if (result.success && parsed.command.kind == pm_tiny::cli::command_kind::quit && result.daemon_pid > 0) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(result.daemon_pid));
        if (process != nullptr) {
            DWORD wait_result = WAIT_TIMEOUT;
            for (int attempt = 0; attempt < 300 && !g_interrupted.load(); ++attempt) {
                wait_result = WaitForSingleObject(process, 100);
                if (wait_result != WAIT_TIMEOUT) break;
            }
            CloseHandle(process);
            if (g_interrupted.load()) return 130;
            if (wait_result == WAIT_TIMEOUT) {
                pm_tiny::win::write_stderr_utf8("pm: timed out waiting for pm_tiny to exit\n");
                return EXIT_FAILURE;
            }
        }
    }
    if (result.success && result.post_list) {
        pm_tiny::cli::parsed_command list_command;
        list_command.kind = pm_tiny::cli::command_kind::list;
        list_command.list_options.terminal_width = pm_tiny::cli::stdout_terminal_width();
        list_command.list_options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
        pm_tiny::cli::cli_response_result list_result;
        if (!send_command(list_command, &list_command.list_options, nullptr, list_result) ||
            !list_result.success) {
            if (!list_result.stderr_text.empty())
                pm_tiny::win::write_stderr_utf8(list_result.stderr_text);
            pm_tiny::win::write_stderr_utf8("pm: command completed, but failed to fetch process list\n");
            return EXIT_FAILURE;
        }
        pm_tiny::win::write_stdout_utf8(list_result.stdout_text);
    }
    return result.success ? EXIT_SUCCESS : EXIT_FAILURE;
}
