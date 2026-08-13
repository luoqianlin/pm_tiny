#include <windows.h>
#include "protocol_v2.h"
#include "frame_stream.hpp"
#include "win_utils.h"
#include "process_list.h"
#include "process_list_renderer.h"
#include "dependency_graph_renderer.h"
#include "connection_error.h"
#include "cli_command.h"

#include <iostream>
#include <cstdint>
#include <string>
#include <vector>

namespace {

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

bool read_pipe_response(HANDLE pipe, std::string &response) {
    response.clear();
    constexpr DWORD kChunkSize = 4096;
    std::vector<char> buffer(kChunkSize);

    while (true) {
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(pipe, buffer.data(), kChunkSize, &bytes_read, nullptr);
        if (!ok) {
            DWORD error = GetLastError();
            if (error == ERROR_MORE_DATA) {
                response.append(buffer.data(), bytes_read);
                continue;
            }
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA) {
                return true;
            }
            std::cerr << "Failed to read response from pm_tiny (error " << error << ")." << std::endl;
            return false;
        }
        if (bytes_read == 0) continue;
        response.append(buffer.data(), bytes_read);
    }
}

bool send_command(const pm_tiny::cli::parsed_command &command, std::string &response,
                  const pm_tiny::cli::list_render_options *list_options,
                  const pm_tiny::cli::dependency_graph_render_options *graph_options) {
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

    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        DWORD error = GetLastError();
        std::cerr << "Failed to configure control pipe (error " << error << ")." << std::endl;
        CloseHandle(pipe);
        return false;
    }

    const auto type = pm_tiny::cli::command_protocol_type(command.kind);
    pm_tiny::protocol_message request;
    request.type = type;
    request.request_id = 1;
    if (command.kind == pm_tiny::cli::command_kind::start) {
        pm_tiny::fappend_value(request.payload, command.start.command);
    } else if (!command.name.empty()) {
        pm_tiny::fappend_value(request.payload, command.name);
    }
    auto payload = pm_tiny::protocol_encode(request);

    DWORD bytes_written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr)) {
        DWORD error = GetLastError();
        std::cerr << "Failed to send command (error " << error << ")." << std::endl;
        CloseHandle(pipe);
        return false;
    }
    FlushFileBuffers(pipe);

    std::string wire_response;
    bool ok = read_pipe_response(pipe, wire_response);
    CloseHandle(pipe);
    if (!ok) return false;
    try {
        pm_tiny::protocol_decoder decoder;
        decoder.feed(reinterpret_cast<const std::uint8_t *>(wire_response.data()), wire_response.size());
        auto message = decoder.pop();
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
        } else {
            response = text;
        }
        if (status != 0 && response.rfind("ERR", 0) != 0) response = "ERR " + response;
        bool received_stream = false;
        while (!decoder.empty()) {
            auto stream_message = decoder.pop();
            if (!(stream_message.flags & pm_tiny::protocol_flag_stream)) {
                throw pm_tiny::protocol_error("unexpected extra response frame");
            }
            pm_tiny::iframe_stream stream_payload(stream_message.payload);
            std::int32_t stream_type = 0;
            std::string chunk;
            stream_payload >> stream_type >> chunk;
            if (!received_stream) {
                response.clear();
                received_stream = true;
            }
            response += chunk;
        }
    } catch (const std::exception &ex) {
        std::cerr << "Invalid response from pm_tiny: " << ex.what() << std::endl;
        return false;
    }
    return ok;
}

} // namespace

int main(int argc, char *argv[]) {
    const auto parsed = pm_tiny::cli::parse_command_line(argc, argv);
    if (!parsed.success) {
        std::cerr << "pm: " << parsed.error << "\n\n"
                  << pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", false);
        return EXIT_FAILURE;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::help) {
        std::cout << pm_tiny::cli::command_usage(argc > 0 ? argv[0] : "pm", false);
        return EXIT_SUCCESS;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::start &&
        parsed.command.start.has_definition_options) {
        std::cerr << "pm: Windows start accepts a configured process name only" << std::endl;
        return EXIT_FAILURE;
    }
    if (parsed.command.kind == pm_tiny::cli::command_kind::restart && parsed.command.show_log) {
        std::cerr << "pm: restart --log is not supported on Windows" << std::endl;
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
    if (!send_command(parsed.command, response,
                      is_list ? &list_options : nullptr,
                      is_graph ? &graph_options : nullptr)) {
        return EXIT_FAILURE;
    }
    if (!response.empty()) {
        std::cout << response;
        if (response.back() != '\n') {
            std::cout << std::endl;
        }
    }
    return EXIT_SUCCESS;
}
