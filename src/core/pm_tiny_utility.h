//
// Created by qianlinluo@foxmail.com on 24-7-31.
//

#ifndef PM_TINY_PM_TINY_UTILITY_H
#define PM_TINY_PM_TINY_UTILITY_H

#include <unistd.h>
#include <memory>

namespace pm_tiny {
    struct CloseableFd;

    void swap(CloseableFd &lhs, CloseableFd &rhs);

    struct CloseableFd {
        int fd_;

        ~CloseableFd() {
            this->close();
        }

        CloseableFd() : fd_(-1) {}

        explicit CloseableFd(int fd) : fd_(fd) {}

        CloseableFd(const CloseableFd &) = delete;

        CloseableFd(CloseableFd &&rhs) noexcept {
            this->fd_ = rhs.fd_;
            rhs.fd_ = -1;
        }

        void close() {
            if (fd_ != -1) {
                ::close(fd_);
                fd_ = -1;
            }
        }

        CloseableFd &operator=(const CloseableFd &) = delete;

        CloseableFd &operator=(CloseableFd &&rhs) noexcept {
            CloseableFd tmp(std::move(rhs));
            swap(tmp, *this);
            return *this;
        }

        explicit operator bool() const {
            return this->fd_ != -1;
        }
    };

  inline  void swap(CloseableFd &lhs, CloseableFd &rhs) {
        std::swap(lhs.fd_, rhs.fd_);
    }
    int get_uid_by_pid(int pid);
}
#endif //PM_TINY_PM_TINY_UTILITY_H
