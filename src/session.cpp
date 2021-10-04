//
// Created by luo on 2021/10/5.
//

#include "session.h"
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <iostream>
#include <utility>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include "pm_sys.h"

#if PM_TINY_SERVER

#include "prog.h"

#endif
namespace pm_tiny {

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

    void session_t::close() {
        if (this->fd_ >= 0) {
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
            ioctl(this->fd_, FIONREAD, &nread);
        }
        if (nread == 0) {
            this->close();
        } else {
            std::unique_ptr<uint8_t[]> s_buf(new uint8_t[nread]);

            auto buf = s_buf.get();
            ssize_t rc;
            rc = safe_read(this->fd_, buf, nread);
            if (rc == 0) {
                this->close();
                return 0;
            }
            if (rc < 0) {
                perror("read");
                return -1;
            }
            nread = (int) rc;
            for (int i = 0; i < nread; i++) {
                if (buf[i] == frame_delimiter) {
                    if (!recv_frame_buf_.empty()) {
                        this->recv_buf.emplace_back(std::make_unique<frame_t>(recv_frame_buf_));
                        recv_frame_buf_.clear();
                    }
                } else {
                    recv_frame_buf_.push_back(buf[i]);
                }
            }
            if (recv_frame_buf_.size() >= frame_max_length) {
                const char *log = "Maximum frame size exceeded, close session!!!\n";
                ::write(STDOUT_FILENO, log, strlen(log));
                this->close();
                return 0;
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
                        break;
                    } else {
                        this->send_buf.pop_front();
                    }
                } else {
                    int fail_errno = errno;
                    std::string errmsg = strerror(errno);
                    if (wbytes == -1) {
                        if (fail_errno == EPIPE) {
                            this->close();
                        } else if (fail_errno != EAGAIN) {
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

    int session_t::rbuf_size() const {
        return (int) this->recv_buf.size();
    }

    bool session_t::sbuf_empty() const {
        return this->sbuf_size() == 0;
    }

    bool session_t::rbuf_empty() const {
        return this->rbuf_size() == 0;
    }

    frame_ptr_t session_t::read_frame(int block) {
        pm_tiny::frame_ptr_t uf;
        do {
            this->read();
        } while (block && !is_close() && this->rbuf_size() < 1);

        if (this->rbuf_size() > 0) {
            auto f = std::move(this->recv_buf.front());
            recv_buf.pop_front();
//            std::cout << "read:" << *f << std::endl;
            uf = std::make_unique<pm_tiny::frame_t>();
            frame_unescape(f->begin(), f->end(), std::back_inserter(*uf));
        }
        return uf;
    }

    frame_ptr_t session_t::get_frame_from_buf() {
        pm_tiny::frame_ptr_t uf;
        if (this->rbuf_size() > 0) {
            auto f = std::move(this->recv_buf.front());
            recv_buf.pop_front();
            uf = std::make_unique<pm_tiny::frame_t>();
            frame_unescape(f->begin(), f->end(), std::back_inserter(*uf));
        }
        return uf;
    }

    int session_t::write_frame(const frame_ptr_t &f, int block) {
        auto wf = std::make_unique<pm_tiny::frame_t>();
        frame_escape(f->begin(), f->end(), std::back_inserter(*wf));
        wf->push_back(frame_delimiter);
//        std::cout << "write:" << *wf << std::endl;
        if (!this->is_close()) {
            this->send_buf.emplace_back(std::move(wf));
            int n = 0;
            do {
                n += this->write();
            } while (block && this->sbuf_size() > 0 && !this->is_close());
            return n;
        } else {
            return -1;
        }
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

#if PM_TINY_SERVER

    void session_t::set_prog(prog_info_t *prog) {
        this->prog_ = prog;
        this->is_new_created_=true;
    }

    prog_info_t *session_t::get_prog() {
        return this->prog_;
    }

#endif

}