#include "client_transport.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace pm_tiny {
namespace sdk_detail {
namespace {

class posix_client_transport final : public client_transport {
public:
    posix_client_transport(std::string endpoint, bool abstract_namespace)
        : endpoint_(std::move(endpoint)), abstract_namespace_(abstract_namespace) {}

    ~posix_client_transport() override { disconnect(); }

    void connect() override {
        if (current_fd() >= 0) return;
        if (cancelled_.load()) throw std::runtime_error("transport cancelled");
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw_system_error("socket");
        if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
            const int error = errno;
            ::close(fd);
            errno = error;
            throw_system_error("fcntl");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        socklen_t length = sizeof(address);
        if (abstract_namespace_) {
            if (endpoint_.size() >= sizeof(address.sun_path)) {
                ::close(fd);
                throw std::runtime_error("abstract UDS name too long");
            }
            address.sun_path[0] = '\0';
            std::memcpy(address.sun_path + 1, endpoint_.data(), endpoint_.size());
            length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint_.size() + 1);
        } else {
            if (endpoint_.size() >= sizeof(address.sun_path)) {
                ::close(fd);
                throw std::runtime_error("UDS path too long");
            }
            std::memcpy(address.sun_path, endpoint_.c_str(), endpoint_.size() + 1);
        }
        const int result = ::connect(fd, reinterpret_cast<sockaddr *>(&address), length);
        if (result < 0 && errno != EINPROGRESS) {
            const int error = errno;
            ::close(fd);
            errno = error;
            throw_system_error("connect");
        }
        set_fd(fd);
        if (result < 0) wait_ready(fd, POLLOUT);
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0) {
            disconnect();
            throw_system_error("getsockopt");
        }
        if (socket_error != 0) {
            disconnect();
            throw std::runtime_error(std::string("connect: ") + std::strerror(socket_error));
        }
    }

    void send(const std::vector<std::uint8_t> &wire) override {
        int fd = current_fd();
        if (fd < 0) throw std::runtime_error("transport is not connected");
        std::size_t offset = 0;
        while (offset < wire.size()) {
            if (cancelled_.load()) throw std::runtime_error("transport cancelled");
            const ssize_t count = ::send(fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                wait_ready(fd, POLLOUT);
                continue;
            }
            throw_system_error("send");
        }
    }

    void disconnect() override {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            fd = fd_;
            fd_ = -1;
        }
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
    }

    void cancel() override {
        cancelled_.store(true);
        disconnect();
    }

private:
    static void throw_system_error(const char *operation) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }

    void wait_ready(int fd, short events) const {
        while (!cancelled_.load()) {
            pollfd descriptor{fd, events, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result > 0) {
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                    throw std::runtime_error("transport connection closed");
                if ((descriptor.revents & events) != 0) return;
            } else if (result < 0 && errno != EINTR) {
                throw_system_error("poll");
            }
        }
        throw std::runtime_error("transport cancelled");
    }

    int current_fd() const {
        std::lock_guard<std::mutex> lock(fd_mutex_);
        return fd_;
    }

    void set_fd(int fd) {
        std::lock_guard<std::mutex> lock(fd_mutex_);
        fd_ = fd;
    }

    std::string endpoint_;
    bool abstract_namespace_ = false;
    std::atomic_bool cancelled_{false};
    mutable std::mutex fd_mutex_;
    int fd_ = -1;
};

} // namespace

std::unique_ptr<client_transport> make_client_transport(
        const std::string &endpoint, bool uds_abstract_namespace) {
    return std::unique_ptr<client_transport>(
            new posix_client_transport(endpoint, uds_abstract_namespace));
}

} // namespace sdk_detail
} // namespace pm_tiny
