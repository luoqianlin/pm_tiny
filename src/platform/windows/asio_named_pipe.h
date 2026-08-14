#pragma once

#include "protocol_v3.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pm_tiny { namespace win {

class AsyncNamedPipeSession;

class AsyncNamedPipeServer {
public:
    class impl;
    using request_handler = std::function<void(
        const std::shared_ptr<AsyncNamedPipeSession> &, const protocol_message &)>;
    using tick_handler = std::function<void()>;
    using process_exit_handler = std::function<void(
        const std::string &, unsigned long long, unsigned long)>;
    using process_log_handler = std::function<void(
        const std::string &, unsigned long long, const char *, std::size_t, bool)>;

    AsyncNamedPipeServer(std::wstring name, request_handler handler, tick_handler tick = {},
                         const std::atomic_bool *running = nullptr);
    ~AsyncNamedPipeServer();
    bool start(std::string &error_message);
    bool poll(std::string &error_message);
    void run_for(unsigned long milliseconds, std::string &error_message);
    bool watch_process(HANDLE process, std::string name, unsigned long long generation,
                       process_exit_handler handler, std::string &error_message);
    bool watch_process_log(HANDLE pipe, std::string name, unsigned long long generation,
                           process_log_handler handler, std::string &error_message);
    void stop();

private:
    std::unique_ptr<impl> impl_;
};

class AsyncNamedPipeSession : public std::enable_shared_from_this<AsyncNamedPipeSession> {
public:
    ~AsyncNamedPipeSession();
    void start();
    void send(protocol_message message);
    void finish();
    void close();
    std::size_t queued_bytes() const;

private:
    friend class AsyncNamedPipeServer::impl;
    class impl;
    explicit AsyncNamedPipeSession(std::unique_ptr<impl> impl);
    std::unique_ptr<impl> impl_;
};

} }
