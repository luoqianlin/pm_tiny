//
// Created by luo on 2021/10/5.
//

#include "session.h"
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <iostream>
#include <cstdio>
#include <utility>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "pm_sys.h"

#if PM_TINY_SERVER

#include "prog.h"

#endif
namespace pm_tiny {

    namespace { constexpr std::size_t session_queue_limit = 1024U * 1024U; }

    std::ostream &operator<<(std::ostream &os, frame_t const &f) {
        std::stringstream ss;
        ss << std::hex << std::uppercase << std::setfill('0');
        for (frame_t::size_type i = 0; i < f.size(); i++) {
            ss << std::setw(2) << static_cast<int>(f[i]) << " ";
        }
        os << ss.str();
        return os;
    }

    session_t::session_t(int fd, int fd_type) {
        this->fd_ = fd;
        this->fd_type_ = fd_type;
        int fsbuf_size;
        int frbuf_size;
        socklen_t scktlen = sizeof(fsbuf_size);
        int rc = getsockopt(this->fd_, SOL_SOCKET, SO_SNDBUF, &fsbuf_size, &scktlen);
        if (rc == -1) {
            perror("getsocketopt");
        }
        rc = getsockopt(this->fd_, SOL_SOCKET, SO_RCVBUF, &frbuf_size, &scktlen);
        if (rc == -1) {
            perror("getsocketopt");
        } else {
            this->fsbuf_size_ = fsbuf_size;
            this->frbuf_size_ = frbuf_size;
        }
        int flags = fcntl(fd, F_GETFL);
        this->fd_nonblock_ = (flags & O_NONBLOCK);
//        printf("fsbuf_size:%d frbuf_size:%d noblock:%d\n",
//               this->fsbuf_size_, this->frbuf_size_, this->fd_nonblock_);
    }

    session_t::~session_t() {
        close();
    }

    void session_t::close() {
        if (this->fd_ >= 0) {
            write_notifier_ = {};
#if PM_TINY_SERVER
            if (prog_) {
                prog_->remove_session(this);
                prog_ = nullptr;
            }
#endif
            ::close(this->fd_);
            this->fd_ = -1;
            this->fd_type_ = 0;
        }
    }


    int session_t::read() {
        int nread = 4096;
        if (this->fd_nonblock_) {
            if (ioctl(this->fd_, FIONREAD, &nread) == -1) {
                return -1;
            }
            if (nread == 0) nread = 4096;
        }
        {
            std::unique_ptr<uint8_t[]> s_buf(new uint8_t[nread]);

            auto buf = s_buf.get();
            ssize_t rc;
            rc = safe_read(this->fd_, buf, nread);
            if (rc == 0) {
                this->close();
                return 0;
            }
            if (rc < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                }
                perror("read");
                return -1;
            }
            nread = (int) rc;
            try {
                decoder_.feed(buf, static_cast<std::size_t>(nread));
                while (!decoder_.empty()) recv_messages_.push_back(decoder_.pop());
            } catch (const protocol_error &ex) {
                std::fprintf(stderr, "protocol decode error: %s\n", ex.what());
                this->close();
                return -1;
            }
        }
        return nread;
    }


    int session_t::write() {
        int total_bytes = 0;
        while (!this->send_buf.empty()
               && total_bytes < this->fsbuf_size_) {
            auto f = this->send_buf.front().get();
            if (!f->empty()) {
                ssize_t wbytes;
                wbytes = safe_send(this->fd_, f->data(), f->size(), MSG_NOSIGNAL);
                if (wbytes > 0) {
                    total_bytes += (int) wbytes;
                    int remaning = (int) (f->size() - wbytes);
                    if (remaning > 0) {
                        f->erase(f->begin(), f->begin() + wbytes);
                        queued_bytes_ -= static_cast<std::size_t>(wbytes);
                        break;
                    } else {
                        queued_bytes_ -= f->size();
                        this->send_buf.pop_front();
                    }
                } else {
                    int fail_errno = errno;
                    std::string errmsg = strerror(errno);
                    if (wbytes == -1) {
                        if (fail_errno == EPIPE) {
                            this->close();
                        } else if (fail_errno != EAGAIN && fail_errno != EWOULDBLOCK) {
                            perror(errmsg.c_str());
                        }
                    }
                    break;
                }
            } else {
                this->send_buf.pop_front();
            }
        }
        return total_bytes;
    }

    bool session_t::is_close() const {
        return this->fd_ < 0;
    }

    int session_t::get_fd() const {
        return this->fd_;
    }

    int session_t::get_fd_type() const {
        return this->fd_type_;
    }

    int session_t::sbuf_size() const {
        return (int) this->send_buf.size();
    }

    std::size_t session_t::queued_bytes() const { return queued_bytes_; }

    int session_t::rbuf_size() const {
        return (int) this->recv_messages_.size();
    }

    bool session_t::sbuf_empty() const {
        return this->sbuf_size() == 0;
    }

    bool session_t::rbuf_empty() const {
        return this->rbuf_size() == 0;
    }

    frame_ptr_t session_t::read_frame(int block) {
        pm_tiny::frame_ptr_t uf;
        while (recv_messages_.empty() && !is_close()) {
            this->read();
            if (!block) break;
        }

        if (!recv_messages_.empty()) {
            auto message = read_message();
            uf = std::make_unique<pm_tiny::frame_t>(message.payload.begin(), message.payload.end());
        }
        return uf;
    }

    protocol_message session_t::read_message(int block) {
        while (recv_messages_.empty() && !is_close()) {
            this->read();
            if (!block) break;
        }
        if (recv_messages_.empty()) return {};
        auto message = std::move(recv_messages_.front());
        recv_messages_.pop_front();
        current_message_type_ = message.type;
        current_request_id_ = message.request_id;
        return message;
    }

    frame_ptr_t session_t::get_frame_from_buf() {
        pm_tiny::frame_ptr_t uf;
        if (!recv_messages_.empty()) {
            auto message = std::move(recv_messages_.front());
            recv_messages_.pop_front();
            uf = std::make_unique<pm_tiny::frame_t>(message.payload.begin(), message.payload.end());
        }
        return uf;
    }

    int session_t::write_frame(const frame_ptr_t &f, int block) {
        protocol_message message;
        message.type = current_message_type_;
        message.flags = protocol_flag_response;
        if (f && f->size() >= sizeof(std::int32_t)) {
            const std::uint32_t status_bits = (static_cast<std::uint32_t>((*f)[0]) << 24) |
                                               (static_cast<std::uint32_t>((*f)[1]) << 16) |
                                               (static_cast<std::uint32_t>((*f)[2]) << 8) |
                                               static_cast<std::uint32_t>((*f)[3]);
            const std::int32_t status = static_cast<std::int32_t>(status_bits);
            if (status != 0) message.flags |= protocol_flag_error;
        }
        message.request_id = current_request_id_;
        message.payload = *f;
        auto encoded = protocol_encode(message);
        auto wf = std::make_unique<pm_tiny::frame_t>(encoded.begin(), encoded.end());
        if (!this->is_close()) {
            if (queued_bytes_ + wf->size() > session_queue_limit) {
                this->close();
                return -1;
            }
            queued_bytes_ += wf->size();
            this->send_buf.emplace_back(std::move(wf));
            int n = 0;
            do {
                n += this->write();
            } while (block && this->sbuf_size() > 0 && !this->is_close());
            if (!this->sbuf_empty() && write_notifier_) write_notifier_();
            return n;
        } else {
            return -1;
        }
    }

    int session_t::write_command_frame(const frame_ptr_t &f, int block,
                                       std::uint32_t request_id, std::uint8_t flags) {
        if (!f || f->empty() || this->is_close()) return -1;
        protocol_message message;
        message.type = (*f)[0];
        message.flags = flags;
        message.request_id = request_id == 0 ? next_request_id_++ : request_id;
        message.payload.assign(f->begin() + 1, f->end());
        auto encoded = protocol_encode(message);
        auto wf = std::make_unique<pm_tiny::frame_t>(encoded.begin(), encoded.end());
        if (queued_bytes_ + wf->size() > session_queue_limit) {
            this->close();
            return -1;
        }
        queued_bytes_ += wf->size();
        this->send_buf.emplace_back(std::move(wf));
        int n = 0;
        do { n += this->write(); } while (block && this->sbuf_size() > 0 && !this->is_close());
        if (!this->sbuf_empty() && write_notifier_) write_notifier_();
        return n;
    }

    int session_t::write_stream_frame(const frame_ptr_t &f, int block, bool more) {
        if (!f || this->is_close()) return -1;
        protocol_message message;
        message.type = current_message_type_;
        message.flags = static_cast<std::uint8_t>(protocol_flag_response | protocol_flag_stream |
                                                  (more ? protocol_flag_more : 0));
        message.request_id = current_request_id_;
        message.payload = *f;
        auto encoded = protocol_encode(message);
        auto wf = std::make_unique<pm_tiny::frame_t>(encoded.begin(), encoded.end());
        if (queued_bytes_ + wf->size() > session_queue_limit) {
            this->close();
            return -1;
        }
        queued_bytes_ += wf->size();
        this->send_buf.emplace_back(std::move(wf));
        int n = 0;
        do { n += this->write(); } while (block && this->sbuf_size() > 0 && !this->is_close());
        if (!this->sbuf_empty() && write_notifier_) write_notifier_();
        return n;
    }

    void session_t::mark_close() {
        mark_closed_ = true;
    }

    bool session_t::is_marked_close() const {
        return mark_closed_;
    }

    int session_t::shutdown_read() {
        this->mark_close();
        return shutdown(fd_, SHUT_RD);
    }

    void session_t::set_write_notifier(std::function<void()> notifier) {
        write_notifier_ = std::move(notifier);
    }

    void session_t::clear_write_notifier() {
        write_notifier_ = {};
    }

    void session_t::set_peer_credentials(pid_t pid, uid_t uid, gid_t gid) {
        has_peer_credentials_ = true;
        peer_pid_ = pid;
        peer_uid_ = uid;
        peer_gid_ = gid;
    }

    bool session_t::has_peer_credentials() const { return has_peer_credentials_; }
    pid_t session_t::peer_pid() const { return peer_pid_; }
    uid_t session_t::peer_uid() const { return peer_uid_; }
    gid_t session_t::peer_gid() const { return peer_gid_; }

#if PM_TINY_SERVER

    void session_t::set_prog(prog_info_t *prog) {
        this->prog_ = prog;
    }

    prog_info_t *session_t::get_prog() {
        return this->prog_;
    }

#endif

}
