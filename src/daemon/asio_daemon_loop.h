#pragma once

#include <functional>

namespace pm_tiny {
class pm_tiny_server_t;

class asio_daemon_loop {
public:
    asio_daemon_loop(pm_tiny_server_t &server, int listen_fd, int signal_fd,
                     std::function<int()> maintenance);
    ~asio_daemon_loop();

    asio_daemon_loop(const asio_daemon_loop &) = delete;
    asio_daemon_loop &operator=(const asio_daemon_loop &) = delete;

    void run();
    void stop();

private:
    class impl;
    impl *impl_;
};
}
