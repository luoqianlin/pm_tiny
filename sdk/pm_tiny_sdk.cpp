#include "pm_tiny_sdk.hpp"

#include "client_transport.h"
#include "frame_stream.hpp"
#include "pm_tiny.h"
#include "protocol_v3.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace pm_tiny {
namespace {

std::string environment_value(const char *name) {
    const char *value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool environment_flag(const char *name) {
    const std::string value = environment_value(name);
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "Y";
}

client_config resolve_config(client_config config) {
    if (config.app_name.empty()) config.app_name = environment_value(PM_TINY_APP_NAME);
#if defined(_WIN32)
    if (config.endpoint.empty()) config.endpoint = environment_value(PM_TINY_PIPE_NAME);
    if (config.endpoint.empty()) config.endpoint = "\\\\.\\pipe\\pm_tiny";
#else
    if (config.endpoint.empty()) config.endpoint = environment_value(PM_TINY_SOCK_FILE);
    if (config.uds_abstract_namespace < 0)
        config.uds_abstract_namespace = environment_flag(PM_TINY_UDS_ABSTRACT_NAMESPACE) ? 1 : 0;
#endif
    return config;
}

} // namespace

class client::impl {
public:
    explicit impl(client_config config)
        : config_(resolve_config(std::move(config))), enabled_(!config_.app_name.empty() && !config_.endpoint.empty()) {
        if (enabled_) {
            transport_ = sdk_detail::make_client_transport(
                    config_.endpoint, config_.uds_abstract_namespace == 1);
            running_ = true;
            worker_ = std::thread(&impl::worker_loop, this);
        }
    }

    ~impl() { close(); }

    enqueue_result enqueue(bool ready_event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return enqueue_result::stopped;
        if (!enabled_) return enqueue_result::disabled;
        bool &pending = ready_event ? pending_ready_ : pending_tick_;
        std::uint64_t &sequence = ready_event ? ready_sequence_ : tick_sequence_;
        std::uint64_t &coalesced = ready_event ? ready_coalesced_ : tick_coalesced_;
        ++sequence;
        if (pending) {
            ++coalesced;
            condition_.notify_one();
            return enqueue_result::coalesced;
        }
        pending = true;
        condition_.notify_one();
        return enqueue_result::queued;
    }

    bool flush(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!enabled_) return true;
        if (stopped_) return !pending_ready_ && !pending_tick_ && !sending_;
        return flushed_.wait_for(lock, timeout, [&]() {
            return !pending_ready_ && !pending_tick_ && !sending_;
        });
    }

    client_status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        client_status result;
        result.enabled = enabled_;
        result.running = running_;
        result.connected = connected_;
        result.pending_ready = pending_ready_;
        result.pending_tick = pending_tick_;
        result.ready_sent = ready_sent_;
        result.tick_sent = tick_sent_;
        result.ready_coalesced = ready_coalesced_;
        result.tick_coalesced = tick_coalesced_;
        result.reconnect_attempts = reconnect_attempts_;
        result.retry_delay_ms = retry_delay_ms_;
        result.app_name = config_.app_name;
        result.endpoint = config_.endpoint;
        result.last_error = last_error_;
        return result;
    }

    void close() {
        std::unique_ptr<sdk_detail::client_transport> *transport = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            running_ = false;
            connected_ = false;
            pending_ready_ = false;
            pending_tick_ = false;
            retry_delay_ms_ = 0;
            transport = &transport_;
        }
        condition_.notify_all();
        flushed_.notify_all();
        if (*transport) (*transport)->cancel();
        if (worker_.joinable()) worker_.join();
    }

private:
    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        std::chrono::milliseconds retry_delay(200);
        while (!stopped_) {
            condition_.wait(lock, [&]() { return stopped_ || pending_ready_ || pending_tick_; });
            if (stopped_) break;
            const bool ready_event = pending_ready_;
            const std::uint64_t sequence = ready_event ? ready_sequence_ : tick_sequence_;
            const std::uint16_t type = ready_event ? PM_TINY_FRAME_TYPE_APP_READY : PM_TINY_FRAME_TYPE_APP_TICK;
            const std::uint32_t request_id = next_request_id_++;
            sending_ = true;
            lock.unlock();
            try {
                transport_->connect();
                protocol_message message;
                message.type = type;
                message.request_id = request_id;
                fappend_value(message.payload, config_.app_name);
                transport_->send(protocol_encode(message));
                lock.lock();
                connected_ = true;
                last_error_.clear();
                retry_delay_ms_ = 0;
                if (ready_event) {
                    ++ready_sent_;
                    if (ready_sequence_ == sequence) pending_ready_ = false;
                } else {
                    ++tick_sent_;
                    if (tick_sequence_ == sequence) pending_tick_ = false;
                }
                sending_ = false;
                retry_delay = std::chrono::milliseconds(200);
                flushed_.notify_all();
            } catch (const std::exception &error) {
                transport_->disconnect();
                lock.lock();
                connected_ = false;
                sending_ = false;
                ++reconnect_attempts_;
                last_error_ = error.what();
                retry_delay_ms_ = static_cast<std::uint32_t>(retry_delay.count());
                flushed_.notify_all();
                condition_.wait_for(lock, retry_delay, [&]() { return stopped_; });
                retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds(5000));
            }
        }
        sending_ = false;
        running_ = false;
        flushed_.notify_all();
    }

    client_config config_;
    const bool enabled_ = false;
    std::unique_ptr<sdk_detail::client_transport> transport_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable flushed_;
    std::thread worker_;
    bool running_ = false;
    bool stopped_ = false;
    bool connected_ = false;
    bool sending_ = false;
    bool pending_ready_ = false;
    bool pending_tick_ = false;
    std::uint64_t ready_sequence_ = 0;
    std::uint64_t tick_sequence_ = 0;
    std::uint64_t ready_sent_ = 0;
    std::uint64_t tick_sent_ = 0;
    std::uint64_t ready_coalesced_ = 0;
    std::uint64_t tick_coalesced_ = 0;
    std::uint64_t reconnect_attempts_ = 0;
    std::uint32_t retry_delay_ms_ = 0;
    std::uint32_t next_request_id_ = 1;
    std::string last_error_;
};

client::client(const client_config &config) : impl_(new impl(config)) {}
client::~client() = default;
client::client(client &&other) noexcept = default;
client &client::operator=(client &&other) noexcept = default;

enqueue_result client::ready() {
    return impl_ ? impl_->enqueue(true) : enqueue_result::stopped;
}
enqueue_result client::tick() {
    return impl_ ? impl_->enqueue(false) : enqueue_result::stopped;
}
bool client::flush(std::chrono::milliseconds timeout) {
    return impl_ ? impl_->flush(timeout) : true;
}
client_status client::status() const {
    return impl_ ? impl_->status() : client_status();
}
void client::close() {
    if (impl_) impl_->close();
}

} // namespace pm_tiny
