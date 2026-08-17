#include "asio_named_pipe.h"
#include "daemon_config.h"

#include <asio.hpp>

#include <array>
#include <cstdlib>
#include <vector>
#include <deque>
#include <iostream>
#include <algorithm>

#include <sddl.h>

namespace pm_tiny { namespace win {

namespace { constexpr std::size_t session_queue_limit = 1024U * 1024U; }

namespace {

std::wstring control_pipe_sddl() {
    const DWORD required = GetEnvironmentVariableW(L"PM_TINY_PIPE_SDDL", nullptr, 0);
    const std::wstring default_sddl(windows_default_pipe_sddl,
                                    windows_default_pipe_sddl + sizeof(windows_default_pipe_sddl) - 1);
    if (required == 0) return default_sddl;
    std::vector<wchar_t> value(required, L'\0');
    if (GetEnvironmentVariableW(L"PM_TINY_PIPE_SDDL", value.data(), required) == 0)
        return default_sddl;
    return std::wstring(value.data());
}

}

class AsyncNamedPipeSession::impl {
public:
    impl(asio::io_context &io_value, HANDLE handle,
         AsyncNamedPipeServer::request_handler handler_value)
        : stream(io_value, handle), handler(std::move(handler_value)) {}

    asio::windows::stream_handle stream;
    AsyncNamedPipeServer::request_handler handler;
    protocol_decoder decoder;
    std::array<std::uint8_t, 4096> read_buffer{};
    std::deque<std::vector<std::uint8_t> > writes;
    std::size_t queued_bytes = 0;
    bool writing = false;
    bool closed = false;
    bool finish_after_writes = false;
    unsigned long long last_activity_ms = GetTickCount64();
};

AsyncNamedPipeSession::AsyncNamedPipeSession(std::unique_ptr<impl> impl_value)
    : impl_(std::move(impl_value)) {}
AsyncNamedPipeSession::~AsyncNamedPipeSession() = default;

void AsyncNamedPipeSession::start() {
    auto self = shared_from_this();
    impl_->stream.async_read_some(asio::buffer(impl_->read_buffer),
        [self](const asio::error_code &error, std::size_t count) {
            if (error) { self->close(); return; }
            self->impl_->last_activity_ms = GetTickCount64();
            try {
                self->impl_->decoder.feed(self->impl_->read_buffer.data(), count);
                while (!self->impl_->decoder.empty())
                    self->impl_->handler(self, self->impl_->decoder.pop());
            } catch (const std::exception &) {
                self->close();
                return;
            }
            self->start();
        });
}

void AsyncNamedPipeSession::send(protocol_message message) {
    if (impl_->closed) return;
    auto wire = protocol_encode(message);
    if (impl_->queued_bytes + wire.size() > session_queue_limit) {
        close();
        return;
    }
    impl_->queued_bytes += wire.size();
    impl_->writes.push_back(std::move(wire));
    impl_->last_activity_ms = GetTickCount64();
    if (impl_->writing) return;
    impl_->writing = true;
    write_next();
}

void AsyncNamedPipeSession::write_next() {
    if (impl_->closed || impl_->writes.empty()) {
        impl_->writing = false;
        if (impl_->finish_after_writes) close();
        return;
    }
    auto self = shared_from_this();
    asio::async_write(impl_->stream, asio::buffer(impl_->writes.front()),
        [self](const asio::error_code &error, std::size_t) {
            if (error) { self->close(); return; }
            self->impl_->queued_bytes -= self->impl_->writes.front().size();
            self->impl_->writes.pop_front();
            self->write_next();
        });
}

std::size_t AsyncNamedPipeSession::queued_bytes() const { return impl_->queued_bytes; }

void AsyncNamedPipeSession::finish() {
    impl_->finish_after_writes = true;
    if (!impl_->writing && impl_->writes.empty()) close();
}

void AsyncNamedPipeSession::close() {
    if (impl_->closed) return;
    impl_->closed = true;
    asio::error_code ignored;
    impl_->stream.cancel(ignored);
    impl_->stream.close(ignored);
}

class AsyncNamedPipeServer::impl {
public:
    struct process_watch {
        process_watch(asio::io_context &io, HANDLE handle_value, std::string name_value,
                      unsigned long long generation_value, process_exit_handler handler_value)
            : handle(io, handle_value), name(std::move(name_value)), generation(generation_value),
              handler(std::move(handler_value)) {}

        asio::windows::object_handle handle;
        std::string name;
        unsigned long long generation;
        process_exit_handler handler;
        bool complete = false;
    };

    struct log_watch : std::enable_shared_from_this<log_watch> {
        log_watch(asio::io_context &io, HANDLE handle_value, std::string name_value,
                  unsigned long long generation_value, process_log_handler handler_value)
            : handle(io, handle_value), name(std::move(name_value)), generation(generation_value),
              handler(std::move(handler_value)) {}

        asio::windows::stream_handle handle;
        std::string name;
        unsigned long long generation;
        process_log_handler handler;
        std::array<char, 16 * 1024> buffer{};
        bool complete = false;

        void read() {
            auto self = shared_from_this();
            handle.async_read_some(asio::buffer(buffer),
                [self](const asio::error_code &error, std::size_t count) {
                    if (!error && count > 0 && self->handler)
                        self->handler(self->name, self->generation, self->buffer.data(), count, false);
                    if (!error) { self->read(); return; }
                    self->complete = true;
                    if (self->handler)
                        self->handler(self->name, self->generation, nullptr, 0, true);
                });
        }
    };

    impl(std::wstring name_value, request_handler handler_value, tick_handler tick_value,
         const std::atomic_bool *running_value)
        : name(std::move(name_value)), handler(std::move(handler_value)), tick(std::move(tick_value)),
          running(running_value) {}

    bool begin_accept(std::string &error) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto sddl = control_pipe_sddl();
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            error = "Invalid PM_TINY_PIPE_SDDL: " + std::to_string(GetLastError());
            return false;
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 5000, &attributes);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            const auto last_error = GetLastError();
            error = "CreateNamedPipe failed: " + std::to_string(last_error);
            if (last_error == ERROR_ACCESS_DENIED) {
                error += " (access denied; verify pm_tiny_pipe_sddl grants the daemon identity access)";
            }
            return false;
        }
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            error = "CreateEvent failed: " + std::to_string(GetLastError());
            CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE;
            return false;
        }
        overlapped = OVERLAPPED{};
        already_connected = false;
        overlapped.hEvent = event;
        const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        already_connected = connected || connect_error == ERROR_PIPE_CONNECTED;
        if (already_connected) SetEvent(event);
        else if (connect_error != ERROR_IO_PENDING) {
            if (connect_error == ERROR_NO_DATA || connect_error == ERROR_PIPE_NOT_CONNECTED ||
                connect_error == ERROR_OPERATION_ABORTED) {
                close_accept();
                return begin_accept(error);
            }
            error = "ConnectNamedPipe failed: " + std::to_string(connect_error);
            close_accept();
            return false;
        }
        HANDLE wait_handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &wait_handle,
                             SYNCHRONIZE, FALSE, 0)) {
            error = "DuplicateHandle for accept event failed: " + std::to_string(GetLastError());
            close_accept();
            return false;
        }
        accept_wait.reset(new asio::windows::object_handle(io, wait_handle));
        accept_wait->async_wait([this](const asio::error_code &wait_error) {
            if (stopped.load() || wait_error) return;
            DWORD transferred = 0;
            if (!already_connected &&
                !GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                const DWORD result = GetLastError();
                if (result != ERROR_PIPE_CONNECTED && result != ERROR_NO_DATA &&
                    result != ERROR_PIPE_NOT_CONNECTED && result != ERROR_OPERATION_ABORTED) {
                    async_error = "ConnectNamedPipe failed: " + std::to_string(result);
                }
                close_accept();
            } else {
                finish_accept();
            }
            if (!stopped.load() && async_error.empty()) begin_accept(async_error);
        });
        return true;
    }

    void finish_accept() {
        HANDLE connected_pipe = pipe;
        pipe = INVALID_HANDLE_VALUE;
        if (accept_wait) {
            accept_wait.reset();
        }
        CloseHandle(event); event = nullptr;
        auto session = std::shared_ptr<AsyncNamedPipeSession>(new AsyncNamedPipeSession(
            std::unique_ptr<AsyncNamedPipeSession::impl>(
                new AsyncNamedPipeSession::impl(io, connected_pipe, handler))));
        sessions.push_back(session);
        session->start();
    }

    void expire_sessions() {
        const auto now = GetTickCount64();
        for (auto it = sessions.begin(); it != sessions.end();) {
            auto session = it->lock();
            if (!session) { it = sessions.erase(it); continue; }
            if (session->impl_->decoder.has_buffered_input() &&
                now - session->impl_->last_activity_ms > 5000) session->close();
            ++it;
        }
    }

    void close_accept() {
        if (accept_wait) {
            asio::error_code ignored;
            accept_wait->cancel(ignored);
            accept_wait.reset();
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIo(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (event != nullptr) { CloseHandle(event); event = nullptr; }
    }

    std::wstring name;
    request_handler handler;
    tick_handler tick;
    const std::atomic_bool *running = nullptr;
    asio::io_context io;
    std::unique_ptr<asio::windows::object_handle> accept_wait;
    std::vector<std::weak_ptr<AsyncNamedPipeSession> > sessions;
    std::vector<std::shared_ptr<process_watch> > process_watches;
    std::vector<std::shared_ptr<log_watch> > log_watches;
    std::atomic_bool stopped{false};
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    bool already_connected = false;
    std::string async_error;
};

AsyncNamedPipeServer::AsyncNamedPipeServer(std::wstring name, request_handler handler,
                                           tick_handler tick, const std::atomic_bool *running)
    : impl_(new impl(std::move(name), std::move(handler), std::move(tick), running)) {}
AsyncNamedPipeServer::~AsyncNamedPipeServer() = default;

bool AsyncNamedPipeServer::start(std::string &error_message) {
    error_message.clear();
    return impl_->pipe != INVALID_HANDLE_VALUE || impl_->begin_accept(error_message);
}

bool AsyncNamedPipeServer::poll(std::string &error_message) {
    error_message.clear();
    impl_->io.restart();
    impl_->io.poll();
    impl_->process_watches.erase(
        std::remove_if(impl_->process_watches.begin(), impl_->process_watches.end(),
                       [](const std::shared_ptr<impl::process_watch> &watch) { return watch->complete; }),
        impl_->process_watches.end());
    impl_->log_watches.erase(
        std::remove_if(impl_->log_watches.begin(), impl_->log_watches.end(),
                       [](const std::shared_ptr<impl::log_watch> &watch) { return watch->complete; }),
        impl_->log_watches.end());
    impl_->expire_sessions();
    error_message = impl_->async_error;
    return error_message.empty();
}

void AsyncNamedPipeServer::run_for(unsigned long milliseconds, std::string &error_message) {
    error_message.clear();
    impl_->io.restart();
    impl_->io.run_one_for(std::chrono::milliseconds(milliseconds));
    impl_->process_watches.erase(
        std::remove_if(impl_->process_watches.begin(), impl_->process_watches.end(),
                       [](const std::shared_ptr<impl::process_watch> &watch) { return watch->complete; }),
        impl_->process_watches.end());
    impl_->log_watches.erase(
        std::remove_if(impl_->log_watches.begin(), impl_->log_watches.end(),
                       [](const std::shared_ptr<impl::log_watch> &watch) { return watch->complete; }),
        impl_->log_watches.end());
    impl_->expire_sessions();
    error_message = impl_->async_error;
}

bool AsyncNamedPipeServer::watch_process(HANDLE process, std::string name,
                                         unsigned long long generation,
                                         process_exit_handler handler,
                                         std::string &error_message) {
    error_message.clear();
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        error_message = "DuplicateHandle failed: " + std::to_string(GetLastError());
        return false;
    }
    auto watch = std::make_shared<impl::process_watch>(
        impl_->io, duplicate, std::move(name), generation, std::move(handler));
    impl_->process_watches.push_back(watch);
    watch->handle.async_wait([watch](const asio::error_code &error) {
        unsigned long exit_code = static_cast<unsigned long>(-1);
        if (!error) GetExitCodeProcess(watch->handle.native_handle(), &exit_code);
        watch->complete = true;
        if (!error && watch->handler) watch->handler(watch->name, watch->generation, exit_code);
    });
    return true;
}

bool AsyncNamedPipeServer::watch_process_log(HANDLE pipe, std::string name,
                                             unsigned long long generation,
                                             process_log_handler handler,
                                             std::string &error_message) {
    error_message.clear();
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        error_message = "invalid log pipe";
        return false;
    }
    auto watch = std::make_shared<impl::log_watch>(
        impl_->io, pipe, std::move(name), generation, std::move(handler));
    impl_->log_watches.push_back(watch);
    watch->read();
    return true;
}

void AsyncNamedPipeServer::stop() {
    if (impl_->stopped.exchange(true)) return;
    impl_->close_accept();
    for (auto &weak : impl_->sessions) if (auto session = weak.lock()) session->close();
    for (auto &watch : impl_->process_watches) {
        asio::error_code ignored;
        watch->handle.cancel(ignored);
        watch->handle.close(ignored);
    }
    impl_->process_watches.clear();
    for (auto &watch : impl_->log_watches) {
        asio::error_code ignored;
        watch->handle.cancel(ignored);
        watch->handle.close(ignored);
    }
    impl_->log_watches.clear();
    impl_->io.stop();
}

} }
