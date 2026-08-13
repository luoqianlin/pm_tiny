#include "asio_named_pipe.h"

#include <asio.hpp>

#include <array>
#include <cstdlib>
#include <vector>

#include <sddl.h>

namespace pm_tiny { namespace win {

class AsioNamedPipe::impl {
public:
    explicit impl(HANDLE handle) : stream(io, handle) {}

    asio::io_context io;
    asio::windows::stream_handle stream;
    protocol_decoder decoder;
};

HANDLE AsioNamedPipe::accept(const std::wstring &name, const std::atomic_bool &running,
                             std::string &error_message) {
    error_message.clear();
    SECURITY_ATTRIBUTES security_attributes{};
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    SECURITY_ATTRIBUTES *security_attributes_ptr = nullptr;
    const char *pipe_sddl = std::getenv("PM_TINY_PIPE_SDDL");
    if (pipe_sddl != nullptr && pipe_sddl[0] != '\0') {
        const std::wstring wide_sddl(pipe_sddl, pipe_sddl + std::char_traits<char>::length(pipe_sddl));
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                wide_sddl.c_str(), SDDL_REVISION_1, &security_descriptor, nullptr)) {
            error_message = "invalid PM_TINY_PIPE_SDDL: " + std::to_string(GetLastError());
            return INVALID_HANDLE_VALUE;
        }
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor = security_descriptor;
        security_attributes.bInheritHandle = FALSE;
        security_attributes_ptr = &security_attributes;
    }
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 5000,
                                   security_attributes_ptr);
    if (security_descriptor != nullptr) LocalFree(security_descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        error_message = "CreateNamedPipe failed: " + std::to_string(GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        error_message = "CreateEvent failed: " + std::to_string(GetLastError());
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error == ERROR_IO_PENDING) {
        while (running.load()) {
            const DWORD wait = WaitForSingleObject(overlapped.hEvent, 250);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                connected = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
                error = connected ? ERROR_SUCCESS : GetLastError();
                break;
            }
            if (wait != WAIT_TIMEOUT) {
                error = GetLastError();
                break;
            }
        }
        if (!running.load()) {
            CancelIo(pipe);
            error = ERROR_OPERATION_ABORTED;
        }
    } else if (!connected && error == ERROR_PIPE_CONNECTED) {
        connected = TRUE;
        error = ERROR_SUCCESS;
    }
    CloseHandle(overlapped.hEvent);
    if (!connected) {
        if (error != ERROR_OPERATION_ABORTED)
            error_message = "ConnectNamedPipe failed: " + std::to_string(error);
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

AsioNamedPipe::AsioNamedPipe(HANDLE handle) : impl_(new impl(handle)) {}
AsioNamedPipe::~AsioNamedPipe() = default;

bool AsioNamedPipe::read_message(protocol_message &message, std::chrono::milliseconds timeout,
                                 std::string &error_message) {
    error_message.clear();
    std::array<std::uint8_t, 4096> buffer{};
    while (impl_->decoder.empty()) {
        impl_->io.restart();
        asio::steady_timer timer(impl_->io);
        asio::error_code read_error;
        std::size_t count = 0;
        timer.expires_after(timeout);
        timer.async_wait([this](const asio::error_code &error) {
            if (!error) impl_->stream.cancel();
        });
        impl_->stream.async_read_some(asio::buffer(buffer),
            [&](const asio::error_code &error, std::size_t bytes) {
                read_error = error;
                count = bytes;
                timer.cancel();
            });
        impl_->io.run();
        if (read_error) {
            error_message = "named pipe read failed: " + read_error.message();
            return false;
        }
        try {
            impl_->decoder.feed(buffer.data(), count);
        } catch (const std::exception &ex) {
            error_message = ex.what();
            return false;
        }
    }
    message = impl_->decoder.pop();
    return true;
}

bool AsioNamedPipe::write_message(const protocol_message &message, std::chrono::milliseconds timeout,
                                  std::string &error_message) {
    error_message.clear();
    const auto wire = protocol_encode(message);
    impl_->io.restart();
    asio::steady_timer timer(impl_->io);
    asio::error_code write_error;
    timer.expires_after(timeout);
    timer.async_wait([this](const asio::error_code &error) {
        if (!error) impl_->stream.cancel();
    });
    asio::async_write(impl_->stream, asio::buffer(wire),
        [&](const asio::error_code &error, std::size_t) {
            write_error = error;
            timer.cancel();
        });
    impl_->io.run();
    if (write_error) {
        error_message = "named pipe write failed: " + write_error.message();
        return false;
    }
    return true;
}

void AsioNamedPipe::cancel() {
    asio::error_code ignored;
    impl_->stream.cancel(ignored);
}

} }
