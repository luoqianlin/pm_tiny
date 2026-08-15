#ifndef PM_TINY_SDK_HPP
#define PM_TINY_SDK_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#if defined(_WIN32)
# if defined(PM_TINY_API_EXPORTS)
#  define PM_TINY_SDK_EXPORT __declspec(dllexport)
# elif defined(PM_TINY_API_IMPORTS)
#  define PM_TINY_SDK_EXPORT __declspec(dllimport)
# else
#  define PM_TINY_SDK_EXPORT
# endif
#elif defined(PM_TINY_API_EXPORTS)
# define PM_TINY_SDK_EXPORT __attribute__((visibility("default")))
#else
# define PM_TINY_SDK_EXPORT
#endif

namespace pm_tiny {

struct client_config {
    std::string app_name;
    std::string endpoint;
    // -1 reads PM_TINY_UDS_ABSTRACT_NAMESPACE, 0 disables it, 1 enables it.
    int uds_abstract_namespace = -1;
};

enum class enqueue_result : std::int32_t {
    queued = 0,
    coalesced = 1,
    disabled = 2,
    stopped = 3,
};

struct client_status {
    bool enabled = false;
    bool running = false;
    bool connected = false;
    bool pending_ready = false;
    bool pending_tick = false;
    std::uint64_t ready_sent = 0;
    std::uint64_t tick_sent = 0;
    std::uint64_t ready_coalesced = 0;
    std::uint64_t tick_coalesced = 0;
    std::uint64_t reconnect_attempts = 0;
    std::uint32_t retry_delay_ms = 0;
    std::string app_name;
    std::string endpoint;
    std::string last_error;
};

class PM_TINY_SDK_EXPORT client {
public:
    explicit client(const client_config &config = client_config());
    ~client();

    client(const client &) = delete;
    client &operator=(const client &) = delete;
    client(client &&other) noexcept;
    client &operator=(client &&other) noexcept;

    enqueue_result ready();
    enqueue_result tick();
    bool flush(std::chrono::milliseconds timeout);
    client_status status() const;
    void close();

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace pm_tiny

#endif
