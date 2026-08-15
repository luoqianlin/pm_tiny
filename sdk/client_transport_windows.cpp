#include "client_transport.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace pm_tiny {
namespace sdk_detail {
namespace {

class windows_client_transport final : public client_transport {
public:
    explicit windows_client_transport(std::string endpoint) : endpoint_(std::move(endpoint)) {}
    ~windows_client_transport() override { disconnect(); }

    void connect() override {
        if (current_handle() != INVALID_HANDLE_VALUE) return;
        if (cancelled_.load()) throw std::runtime_error("transport cancelled");
        HANDLE handle = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("named pipe connect failed: " + std::to_string(GetLastError()));
        }
        DWORD mode = PIPE_READMODE_BYTE;
        if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            throw std::runtime_error("named pipe mode failed: " + std::to_string(error));
        }
        set_handle(handle);
    }

    void send(const std::vector<std::uint8_t> &wire) override {
        HANDLE handle = current_handle();
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("transport is not connected");
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) throw std::runtime_error("CreateEvent failed");
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        DWORD written = 0;
        const BOOL started = WriteFile(handle, wire.data(), static_cast<DWORD>(wire.size()),
                                       &written, &overlapped);
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            const DWORD error = GetLastError();
            CloseHandle(event);
            throw std::runtime_error("named pipe write failed: " + std::to_string(error));
        }
        while (!started) {
            const DWORD wait = WaitForSingleObject(event, 100);
            if (wait == WAIT_OBJECT_0) break;
            if (cancelled_.load()) {
                CancelIoEx(handle, &overlapped);
                CloseHandle(event);
                throw std::runtime_error("transport cancelled");
            }
            if (wait != WAIT_TIMEOUT) {
                CancelIoEx(handle, &overlapped);
                CloseHandle(event);
                throw std::runtime_error("named pipe wait failed");
            }
        }
        if (!started && !GetOverlappedResult(handle, &overlapped, &written, FALSE)) {
            const DWORD error = GetLastError();
            CloseHandle(event);
            throw std::runtime_error("named pipe write failed: " + std::to_string(error));
        }
        CloseHandle(event);
        if (written != static_cast<DWORD>(wire.size()))
            throw std::runtime_error("short named pipe write");
    }

    void disconnect() override {
        HANDLE handle = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(handle_mutex_);
            handle = handle_;
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle, nullptr);
            CloseHandle(handle);
        }
    }

    void cancel() override {
        cancelled_.store(true);
        disconnect();
    }

private:
    HANDLE current_handle() const {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        return handle_;
    }
    void set_handle(HANDLE handle) {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        handle_ = handle;
    }

    std::string endpoint_;
    std::atomic_bool cancelled_{false};
    mutable std::mutex handle_mutex_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

} // namespace

std::unique_ptr<client_transport> make_client_transport(
        const std::string &endpoint, bool) {
    return std::unique_ptr<client_transport>(new windows_client_transport(endpoint));
}

} // namespace sdk_detail
} // namespace pm_tiny
