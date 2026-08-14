#include "AppClient.h"

#include "asio_protocol_client.h"
#include "frame_stream.hpp"
#include "pm_tiny.h"
#include "protocol_v3.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

namespace pm_tiny {

class AppClient::AppClientImpl {
public:
    AppClientImpl() : app_name_(read_env(PM_TINY_APP_NAME)), pipe_name_(read_env("PM_TINY_PIPE_NAME")) {
        if (pipe_name_.empty()) pipe_name_ = "\\\\.\\pipe\\pm_tiny";
    }

    bool is_enable() const { return !app_name_.empty() && !pipe_name_.empty(); }
    std::string get_app_name() const { return app_name_; }
    void tick() { send(PM_TINY_FRAME_TYPE_APP_TICK); }
    void ready() { send(PM_TINY_FRAME_TYPE_APP_READY); }

private:
    static std::string read_env(const char *name) {
        const int name_size = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
        if (name_size <= 0) return {};
        std::wstring wide_name(static_cast<std::size_t>(name_size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, &wide_name[0], name_size) <= 0) return {};
        const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        if (required == 0) return {};
        std::wstring wide_value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), &wide_value[0], required);
        if (written == 0 || written >= required) return {};
        wide_value.resize(written);
        const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide_value.data(),
                                                   static_cast<int>(wide_value.size()),
                                                   nullptr, 0, nullptr, nullptr);
        if (utf8_size <= 0) return {};
        std::string result(static_cast<std::size_t>(utf8_size), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide_value.data(),
                                static_cast<int>(wide_value.size()), &result[0],
                                utf8_size, nullptr, nullptr) <= 0) return {};
        return result;
    }

    void send(std::uint16_t type) {
        if (!is_enable()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_message request;
        request.type = type;
        request.request_id = next_request_id_++;
        fappend_value(request.payload, app_name_);
        try {
            asio_protocol_client client(pipe_name_);
            client.send(request);
        } catch (...) {
            return;
        }
    }

    std::string app_name_;
    std::string pipe_name_;
    std::mutex mutex_;
    std::uint32_t next_request_id_ = 1;
};

AppClient::AppClient() : impl_(new AppClientImpl()) {}
AppClient::~AppClient() = default;
bool AppClient::is_enable() const { return impl_->is_enable(); }
std::string AppClient::get_app_name() const { return impl_->get_app_name(); }
void AppClient::tick() const { impl_->tick(); }
void AppClient::ready() const { impl_->ready(); }

} // namespace pm_tiny
