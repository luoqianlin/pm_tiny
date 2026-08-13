#include "asio_daemon_loop.h"

#include "pm_tiny_server.h"
#include "pm_tiny_funcs.h"
#include "prog.h"
#include "session.h"

#include <asio.hpp>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace pm_tiny {

class asio_daemon_loop::impl {
public:
    impl(pm_tiny_server_t &server, int listen_fd, std::function<bool()> maintenance)
        : server_(server), io_(), listen_fd_(listen_fd), maintenance_(std::move(maintenance)),
          listen_wait_(io_, ::dup(listen_fd)) {}

    ~impl() {
        stop();
    }

    void run() {
        if (listen_wait_.native_handle() < 0) return;
        wait_listen();
        arm_timer();
        io_.run();
    }

    void stop() {
        if (stopped_) return;
        stopped_ = true;
        io_.stop();
        asio::error_code ignored;
        listen_wait_.close(ignored);
        timer_.cancel();
        for (auto &entry : sessions_) entry.second->close(ignored);
        sessions_.clear();
        for (auto &entry : pipes_) entry.second->close(ignored);
        pipes_.clear();
    }

private:
    using descriptor_ptr = std::shared_ptr<asio::posix::stream_descriptor>;

    void wait_listen() {
        listen_wait_.async_wait(asio::posix::stream_descriptor::wait_read,
                                [this](const asio::error_code &error) {
            if (stopped_ || error) return;
            for (;;) {
                sockaddr_un peer{};
                socklen_t length = sizeof(peer);
                const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &length,
                                         SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                auto session = std::make_shared<session_t>(fd, 0);
                server_.sessions.emplace_back(session);
                arm_session(session);
            }
            wait_listen();
        });
    }

    void arm_session(const session_ptr_t &session) {
        if (stopped_ || !session || session->is_close()) return;
        auto iter = sessions_.find(session->get_fd());
        if (iter == sessions_.end()) {
            const int duplicate = ::dup(session->get_fd());
            if (duplicate < 0) return;
            auto descriptor = std::make_shared<asio::posix::stream_descriptor>(io_, duplicate);
            iter = sessions_.emplace(session->get_fd(), descriptor).first;
        }
        auto descriptor = iter->second;
        const auto fd = session->get_fd();
        descriptor->async_wait(asio::posix::stream_descriptor::wait_read,
                               [this, session, descriptor, fd](const asio::error_code &error) {
            if (stopped_ || error) return;
            if (!session->is_close()) {
                handle_session_read(session);
            }
            if (session->is_close()) {
                sessions_.erase(fd);
                return;
            }
            if (session->sbuf_size() > 0) {
                descriptor->async_wait(asio::posix::stream_descriptor::wait_write,
                    [this, session, descriptor, fd](const asio::error_code &write_error) {
                        if (stopped_ || write_error) return;
                        session->write();
                        if (session->is_close()) sessions_.erase(fd);
                        else arm_session(session);
                    });
            } else {
                arm_session(session);
            }
        });
    }

    void handle_session_read(session_ptr_t session) {
        auto message = session->read_message();
        if (message.type == 0 || session->is_close()) return;
        try {
            handle_frame(server_, message, session);
        } catch (const BufferInsufficientException &) {
            auto error = std::make_unique<frame_t>();
            fappend_value<std::int32_t>(*error, -1);
            fappend_value(*error, std::string("Invalid argument"));
            session->write_frame(error);
            session->shutdown_read();
        }
    }

    void refresh_pipes() {
        std::unordered_map<int, bool> active;
        for (auto &program : server_.pm_tiny_progs) {
            for (int index = 0; index < 2; ++index) {
                const int fd = program->rpipefd[index];
                if (fd < 0) continue;
                active[fd] = true;
                if (pipes_.find(fd) == pipes_.end()) {
                    const int duplicate = ::dup(fd);
                    if (duplicate < 0) continue;
                    auto descriptor = std::make_shared<asio::posix::stream_descriptor>(io_, duplicate);
                    pipes_.emplace(fd, descriptor);
                    arm_pipe(program, index, fd, descriptor);
                }
            }
        }
        for (auto iter = pipes_.begin(); iter != pipes_.end();) {
            if (!active.count(iter->first)) {
                asio::error_code ignored;
                iter->second->close(ignored);
                iter = pipes_.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    void arm_pipe(const prog_ptr_t &program, int index, int fd, const descriptor_ptr &descriptor) {
        const uint64_t generation = program->instance.generation;
        const std::weak_ptr<const char> lifetime = program->lifetime_token;
        descriptor->async_wait(asio::posix::stream_descriptor::wait_read,
                               [this, program, index, fd, generation, descriptor, lifetime](const asio::error_code &error) {
            if (stopped_ || error) return;
            if (lifetime.expired() || program->instance.generation != generation) {
                auto iter = pipes_.find(fd);
                if (iter != pipes_.end() && iter->second == descriptor) pipes_.erase(iter);
                asio::error_code ignored;
                descriptor->close(ignored);
                return;
            }
            if (program->rpipefd[index] >= 0) program->read_pipe(index);
            if (!stopped_ && program->rpipefd[index] == fd) arm_pipe(program, index, fd, descriptor);
        });
    }

    void arm_timer() {
        timer_.expires_after(std::chrono::milliseconds(100));
        timer_.async_wait([this](const asio::error_code &error) {
            if (stopped_ || error) return;
            refresh_pipes();
            if (maintenance_ && maintenance_()) {
                stop();
                return;
            }
            for (auto &session : server_.sessions) {
                if (session && !session->is_close() && session->sbuf_size() > 0) arm_session(session);
            }
            arm_timer();
        });
    }

    pm_tiny_server_t &server_;
    asio::io_context io_;
    int listen_fd_;
    std::function<bool()> maintenance_;
    asio::posix::stream_descriptor listen_wait_;
    asio::steady_timer timer_{io_};
    std::unordered_map<int, descriptor_ptr> sessions_;
    std::unordered_map<int, descriptor_ptr> pipes_;
    bool stopped_ = false;
};

asio_daemon_loop::asio_daemon_loop(pm_tiny_server_t &server, int listen_fd,
                                   std::function<bool()> maintenance)
    : impl_(new impl(server, listen_fd, std::move(maintenance))) {}

asio_daemon_loop::~asio_daemon_loop() { delete impl_; }
void asio_daemon_loop::run() { impl_->run(); }
void asio_daemon_loop::stop() { impl_->stop(); }

} // namespace pm_tiny
