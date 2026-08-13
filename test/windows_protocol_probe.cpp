#include "frame_stream.hpp"
#include "protocol_v2.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring pipe_name() {
    const char *configured = std::getenv("PM_TINY_PIPE_NAME");
    const std::string name = configured != nullptr && configured[0] != '\0'
                             ? configured : "\\\\.\\pipe\\pm_tiny";
    const int count = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (count <= 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<std::size_t>(count));
    if (MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, buffer.data(), count) <= 0) {
        return std::wstring();
    }
    return std::wstring(buffer.data());
}

HANDLE connect_pipe() {
    const auto name = pipe_name();
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return pipe;
            CloseHandle(pipe);
            return INVALID_HANDLE_VALUE;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return INVALID_HANDLE_VALUE;
}

bool write_all(HANDLE pipe, const std::uint8_t *data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, data + offset, static_cast<DWORD>(size - offset), &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool wait_for_disconnect(HANDLE pipe, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool read_response(HANDLE pipe, pm_tiny::protocol_message &response) {
    pm_tiny::protocol_decoder decoder;
    std::vector<std::uint8_t> buffer(4096);
    while (decoder.empty()) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (read == 0) continue;
        decoder.feed(buffer.data(), read);
    }
    response = decoder.pop();
    return true;
}

bool response_status(const pm_tiny::protocol_message &response, std::int32_t &status, std::string &text) {
    try {
        pm_tiny::iframe_stream stream(response.payload);
        stream >> status >> text;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::uint8_t> version_request(std::uint16_t type = 0x29) {
    pm_tiny::protocol_message request;
    request.type = type;
    request.request_id = 0x10203040;
    return pm_tiny::protocol_encode(request);
}

int run_fragmented() {
    HANDLE pipe = connect_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    const auto request = version_request();
    for (const auto byte : request) {
        if (!write_all(pipe, &byte, 1)) {
            CloseHandle(pipe);
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pm_tiny::protocol_message response;
    const bool read = read_response(pipe, response);
    CloseHandle(pipe);
    std::int32_t status = -1;
    std::string text;
    if (!read || response.type != 0x29 || response.request_id != 0x10203040 ||
        !response_status(response, status, text) || status != 0 || text.find("Windows v2") == std::string::npos) {
        return 4;
    }
    return 0;
}

int run_unknown_type() {
    HANDLE pipe = connect_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    const auto request = version_request(0x7fff);
    if (!write_all(pipe, request.data(), request.size())) {
        CloseHandle(pipe);
        return 3;
    }
    pm_tiny::protocol_message response;
    const bool read = read_response(pipe, response);
    CloseHandle(pipe);
    std::int32_t status = 0;
    std::string text;
    if (!read || !(response.flags & pm_tiny::protocol_flag_error) ||
        !response_status(response, status, text) || status >= 0 || text.find("unknown message type") == std::string::npos) {
        return 4;
    }
    return 0;
}

int run_rejected(const std::string &mode) {
    HANDLE pipe = connect_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    auto request = version_request();
    if (mode == "invalid-magic") {
        request[0] = 'X';
    } else if (mode == "invalid-version") {
        request[4] = 99;
    } else if (mode == "invalid-flags") {
        request[5] = 0x80;
    } else if (mode == "oversize") {
        const std::uint32_t size = pm_tiny::protocol_v2_max_payload + 1;
        request[12] = static_cast<std::uint8_t>(size >> 24);
        request[13] = static_cast<std::uint8_t>(size >> 16);
        request[14] = static_cast<std::uint8_t>(size >> 8);
        request[15] = static_cast<std::uint8_t>(size);
    } else {
        CloseHandle(pipe);
        return 5;
    }
    const bool written = write_all(pipe, request.data(), request.size());
    const bool disconnected = written && wait_for_disconnect(pipe, std::chrono::seconds(3));
    CloseHandle(pipe);
    return disconnected ? 0 : 4;
}

int run_slow() {
    HANDLE pipe = connect_pipe();
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    const auto request = version_request();
    if (!write_all(pipe, request.data(), 8)) {
        CloseHandle(pipe);
        return 3;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));
    const bool disconnected = wait_for_disconnect(pipe, std::chrono::seconds(2));
    CloseHandle(pipe);
    return disconnected ? 0 : 4;
}

int run_coalesced_decoder() {
    const auto first = version_request();
    const auto second = version_request(0x23);
    std::vector<std::uint8_t> wire = first;
    wire.insert(wire.end(), second.begin(), second.end());
    pm_tiny::protocol_decoder decoder;
    decoder.feed(wire.data(), wire.size());
    if (decoder.empty() || decoder.pop().type != 0x29 || decoder.empty() || decoder.pop().type != 0x23 ||
        !decoder.empty()) {
        return 4;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: windows_protocol_probe <mode>" << std::endl;
        return 1;
    }
    const std::string mode = argv[1];
    int result = 1;
    if (mode == "fragmented") result = run_fragmented();
    else if (mode == "unknown-type") result = run_unknown_type();
    else if (mode == "slow") result = run_slow();
    else if (mode == "coalesced") result = run_coalesced_decoder();
    else result = run_rejected(mode);
    if (result != 0) std::cerr << "probe mode failed: " << mode << " (" << result << ")" << std::endl;
    return result;
}
