#include "asio_daemon_loop.h"

#include "pm_tiny_server.h"
#include "pm_tiny_funcs.h"
#include "prog.h"
#include "session.h"
#include "daemon_log.h"

#include <asio.hpp>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace pm_tiny {

namespace {
constexpr std::size_t max_control_sessions = 256;
constexpr std::size_t max_messages_per_dispatch = 32;

bool is_accept_resource_error(int error) {
    return error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM;
}
}

class asio_daemon_loop::impl {
public:
    impl(pm_tiny_server_t &server, int listen_fd, int signal_fd, std::function<int()> maintenance)
        : server_(server), io_(), listen_fd_(listen_fd), maintenance_(std::move(maintenance)),
          listen_wait_(io_, ::dup(listen_fd)),
          signal_wait_(io_, signal_fd >= 0 ? ::dup(signal_fd) : -1),
          accept_retry_(io_) {}

    ~impl() {
        stop();
    }

    void run() {
        if (listen_wait_.native_handle() < 0) return;
        wait_listen();
        wait_signal();
        run_maintenance();
        io_.run();
    }

    void stop() {
        if (stopped_) return;
        stopped_ = true;
        io_.stop();
        asio::error_code ignored;
        listen_wait_.close(ignored);
        signal_wait_.close(ignored);
        timer_.cancel();
        for (auto &entry : sessions_) {
            entry.second->removed = true;
            entry.second->session->clear_write_notifier();
            entry.second->descriptor->close(ignored);
            entry.second->session->close();
        }
        sessions_.clear();
        for (auto &entry : pipes_) entry.second->close(ignored);
        pipes_.clear();
        accept_retry_.cancel();
    }

private:
    using descriptor_ptr = std::shared_ptr<asio::posix::stream_descriptor>;

    struct session_state {
        session_ptr_t session;
        descriptor_ptr descriptor;
        int fd = -1;
        bool read_wait = false;
        bool write_wait = false;
        bool dispatch_posted = false;
        bool removed = false;
    };

    using session_state_ptr = std::shared_ptr<session_state>;

    void remove_session(const session_state_ptr &state) {
        if (!state || state->removed) return;
        state->removed = true;
        state->session->clear_write_notifier();
        auto found = sessions_.find(state->fd);
        if (found != sessions_.end() && found->second == state) sessions_.erase(found);
        asio::error_code ignored;
        state->descriptor->close(ignored);
        if (!state->session->is_close()) state->session->close();
        auto &owned = server_.sessions;
        owned.erase(std::remove(owned.begin(), owned.end(), state->session), owned.end());
    }

    void wait_listen() {
        if (stopped_ || listen_wait_active_) return;
        listen_wait_active_ = true;
        listen_wait_.async_wait(asio::posix::stream_descriptor::wait_read,
                                [this](const asio::error_code &error) {
            listen_wait_active_ = false;
            if (stopped_ || error) return;
            bool retry_later = false;
            for (;;) {
                sockaddr_un peer{};
                socklen_t length = sizeof(peer);
                const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &length,
                                         SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (is_accept_resource_error(errno)) {
                        PM_TINY_DLOG_ERROR_ERRNO("accept control connection");
                        retry_later = true;
                    }
                    break;
                }
                if (server_.sessions.size() >= max_control_sessions) {
                    PM_TINY_DLOG_ERROR("reject control connection: session limit %zu reached",
                                       max_control_sessions);
                    ::close(fd);
                    continue;
                }
#ifdef SO_PEERCRED
                struct ucred credential{};
                socklen_t credential_length = sizeof(credential);
                if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &credential_length) != 0 ||
                    !peer_allowed(credential.uid, credential.gid)) {
                    PM_TINY_DLOG_ERROR("reject control connection pid=%d uid=%u gid=%u",
                                  static_cast<int>(credential.pid),
                                  static_cast<unsigned int>(credential.uid),
                                  static_cast<unsigned int>(credential.gid));
                    ::close(fd);
                    continue;
                }
#endif
                auto session = std::make_shared<session_t>(fd, 0);
#ifdef SO_PEERCRED
                session->set_peer_credentials(credential.pid, credential.uid, credential.gid);
#endif
                const int duplicate = ::dup(fd);
                if (duplicate < 0) {
                    PM_TINY_DLOG_ERROR_ERRNO("duplicate control connection");
                    session->close();
                    continue;
                }
                auto state = std::make_shared<session_state>();
                state->session = session;
                state->descriptor = std::make_shared<asio::posix::stream_descriptor>(io_, duplicate);
                state->fd = fd;
                sessions_.emplace(fd, state);
                std::weak_ptr<session_state> weak_state = state;
                session->set_write_notifier([this, weak_state]() {
                    asio::post(io_, [this, weak_state]() {
                        if (auto state = weak_state.lock()) arm_session_write(state);
                    });
                });
                server_.sessions.emplace_back(session);
                arm_session_read(state);
            }
            if (retry_later) {
                accept_retry_.expires_after(std::chrono::milliseconds(250));
                accept_retry_.async_wait([this](const asio::error_code &retry_error) {
                    if (!stopped_ && !retry_error) wait_listen();
                });
            } else {
                wait_listen();
            }
        });
    }

    bool peer_allowed(unsigned int uid, unsigned int gid) const {
        if (uid == static_cast<unsigned int>(::geteuid())) return true;
        if (std::find(server_.allowed_uids.begin(), server_.allowed_uids.end(), uid) !=
            server_.allowed_uids.end()) return true;
        return std::find(server_.allowed_gids.begin(), server_.allowed_gids.end(), gid) !=
               server_.allowed_gids.end();
    }

    void wait_signal() {
        if (stopped_ || signal_wait_.native_handle() < 0) return;
        signal_wait_.async_wait(asio::posix::stream_descriptor::wait_read,
                                [this](const asio::error_code &error) {
            if (stopped_ || error) return;
            char buffer[64];
            while (::read(signal_wait_.native_handle(), buffer, sizeof(buffer)) > 0) {}
            run_maintenance();
            wait_signal();
        });
    }

    void arm_session_read(const session_state_ptr &state) {
        if (stopped_ || !state || state->removed || state->read_wait) return;
        auto session = state->session;
        if (session->is_close()) {
            remove_session(state);
            return;
        }
        if (session->is_marked_close()) {
            if (session->sbuf_empty()) remove_session(state);
            else arm_session_write(state);
            return;
        }
        state->read_wait = true;
        state->descriptor->async_wait(asio::posix::stream_descriptor::wait_read,
                               [this, state](const asio::error_code &error) {
            state->read_wait = false;
            if (stopped_ || state->removed) return;
            if (error) {
                remove_session(state);
                return;
            }
            const int rc = state->session->read();
            if (rc < 0 || state->session->is_close()) {
                remove_session(state);
                return;
            }
            dispatch_messages(state);
            arm_session_write(state);
            arm_session_read(state);
        });
    }

    void arm_session_write(const session_state_ptr &state) {
        if (stopped_ || !state || state->removed || state->write_wait) return;
        auto session = state->session;
        if (session->is_close()) {
            remove_session(state);
            return;
        }
        if (session->sbuf_empty()) {
            if (session->is_marked_close()) remove_session(state);
            return;
        }
        state->write_wait = true;
        state->descriptor->async_wait(asio::posix::stream_descriptor::wait_write,
                                      [this, state](const asio::error_code &error) {
            state->write_wait = false;
            if (stopped_ || state->removed) return;
            if (error) {
                remove_session(state);
                return;
            }
            state->session->write();
            if (state->session->is_close() ||
                (state->session->is_marked_close() && state->session->sbuf_empty())) {
                remove_session(state);
                return;
            }
            arm_session_write(state);
        });
    }

    void dispatch_messages(const session_state_ptr &state) {
        if (!state || state->removed || state->session->is_close()) return;
        std::size_t handled = 0;
        while (handled < max_messages_per_dispatch && !state->session->rbuf_empty() &&
               !state->session->is_marked_close() && !state->session->is_close()) {
            auto message = state->session->read_message();
            if (message.type == 0) break;
            try {
                handle_frame(server_, message, state->session);
            } catch (const BufferInsufficientException &) {
                auto error = std::make_unique<frame_t>();
                fappend_value<std::int32_t>(*error, -1);
                fappend_value(*error, std::string("Invalid argument"));
                state->session->write_frame(error);
                state->session->shutdown_read();
            }
            ++handled;
            if (state->session->is_close()) break;
        }
        arm_session_write(state);
        if (!state->session->rbuf_empty() && !state->session->is_marked_close() &&
            !state->dispatch_posted) {
            state->dispatch_posted = true;
            asio::post(io_, [this, state]() {
                state->dispatch_posted = false;
                if (stopped_ || state->removed) return;
                dispatch_messages(state);
                arm_session_read(state);
            });
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

    void arm_timer(int delay_ms) {
        timer_.cancel();
        timer_.expires_after(std::chrono::milliseconds(std::max(1, delay_ms)));
        timer_.async_wait([this](const asio::error_code &error) {
            if (stopped_ || error) return;
            run_maintenance();
        });
    }

    void run_maintenance() {
        if (stopped_) return;
        refresh_pipes();
        std::vector<session_state_ptr> expired_sessions;
        for (const auto &entry : sessions_) {
            const auto &state = entry.second;
            if (state->session->is_close() ||
                (state->session->is_marked_close() && state->session->sbuf_empty())) {
                expired_sessions.push_back(state);
            }
        }
        for (const auto &state : expired_sessions) remove_session(state);
        auto &owned = server_.sessions;
        owned.erase(std::remove_if(owned.begin(), owned.end(), [](const session_ptr_t &session) {
            return !session || session->is_close();
        }), owned.end());
        for (const auto &entry : sessions_) {
            arm_session_write(entry.second);
            arm_session_read(entry.second);
        }
        const int next_delay = maintenance_ ? maintenance_() : 1000;
        if (next_delay < 0) {
            stop();
            return;
        }
        arm_timer(next_delay);
    }

    pm_tiny_server_t &server_;
    asio::io_context io_;
    int listen_fd_;
    std::function<int()> maintenance_;
    asio::posix::stream_descriptor listen_wait_;
    asio::posix::stream_descriptor signal_wait_;
    asio::steady_timer timer_{io_};
    asio::steady_timer accept_retry_;
    std::unordered_map<int, session_state_ptr> sessions_;
    std::unordered_map<int, descriptor_ptr> pipes_;
    bool listen_wait_active_ = false;
    bool stopped_ = false;
};

asio_daemon_loop::asio_daemon_loop(pm_tiny_server_t &server, int listen_fd, int signal_fd,
                                   std::function<int()> maintenance)
    : impl_(new impl(server, listen_fd, signal_fd, std::move(maintenance))) {}

asio_daemon_loop::~asio_daemon_loop() { delete impl_; }
void asio_daemon_loop::run() { impl_->run(); }
void asio_daemon_loop::stop() { impl_->stop(); }

} // namespace pm_tiny
