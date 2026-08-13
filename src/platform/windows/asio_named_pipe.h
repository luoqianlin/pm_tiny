#pragma once

#include "protocol_v2.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace pm_tiny { namespace win {

class AsioNamedPipe {
public:
    static HANDLE accept(const std::wstring &name, const std::atomic_bool &running,
                         std::string &error_message);

    explicit AsioNamedPipe(HANDLE handle);
    ~AsioNamedPipe();

    AsioNamedPipe(const AsioNamedPipe &) = delete;
    AsioNamedPipe &operator=(const AsioNamedPipe &) = delete;

    bool read_message(protocol_message &message, std::chrono::milliseconds timeout,
                      std::string &error_message);
    bool write_message(const protocol_message &message, std::chrono::milliseconds timeout,
                       std::string &error_message);
    void cancel();

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} }
