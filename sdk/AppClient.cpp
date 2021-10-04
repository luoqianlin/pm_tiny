//
// Created by qianlinluo@foxmail.com on 23-7-11.
//

#include "AppClient.h"
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <memory>
#include <iostream>
#include <string>
#include "frame_stream.hpp"
#include "session.h"
#include "pm_tiny.h"
#include <thread>
#include <atomic>
#include <condition_variable>

#include <chrono>
#include <mutex>
#include "strtools.h"

#define PM_TINY_CLIENT_CONNECT_TIMEOUT 0

#define   THROW_EXCEPTION_WITH_ERROR(name)  do {\
                    char error_msg[1024];\
                    char fmt_buff[2048];\
                    auto rc=strerror_r(errno, error_msg, sizeof(error_msg)); \
                    snprintf(fmt_buff, sizeof(fmt_buff), "%s:%d %s:%s", PM_TINY_SHORT_FILE, __LINE__,name, error_msg);\
                    throw std::runtime_error(error_msg);\
                } while (false)

namespace pm_tiny {
    class AppClient::AppClientImpl {
    public:
        struct Message {
            explicit Message(byte_t type) : type_(type) {}

            byte_t type_;
        };

        AppClientImpl() {
            this->app_name_ = get_app_name_();
            this->pm_tiny_addr_ = get_pm_tiny_addr_();
            this->uds_abstract_namespace_ = get_pm_tiny_uds_abstract_namespace_();
            if (this->is_enable()) {
                this->msg_thread_start_ = true;
                msg_thread_ = std::thread(&AppClient::AppClientImpl::send_msg_loop, this);
            }
            
        }

        ~AppClientImpl() {
            this->msg_thread_start_ = false;
            this->send_msg_cond_var_.notify_one();
            if (msg_thread_.joinable()) {
                msg_thread_.join();
            }

        }

        bool is_enable() const {
            return !this->app_name_.empty() && !this->pm_tiny_addr_.empty();
        }

        std::string get_app_name() const {
            return this->app_name_;
        }

        void tick() {
            if (!is_enable())return;
            send_msg(PM_TINY_FRAME_TYPE_APP_TICK);
        }

        void ready() {
            if (!is_enable())return;
            send_msg(PM_TINY_FRAME_TYPE_APP_READY);
        }

    private:
        void send_msg(byte_t type) {
            using namespace std::chrono;
            {
                std::lock_guard<std::mutex> guard{this->msg_mutex_};
                if (std::chrono::steady_clock::now() - time_point_ < 200ms)return;
                if (send_msg_queue_.size() >= send_msg_queue_size) {
                    return;
                }
                send_msg_queue_.emplace(type);
                time_point_ = std::chrono::steady_clock::now();
            }
            send_msg_cond_var_.notify_one();
            
        }

        void send_msg_loop() {
            try {
                session_ = make_session(pm_tiny_addr_,uds_abstract_namespace_);
                auto app_name = get_app_name();
                while (this->msg_thread_start_) {
                    std::unique_lock<std::mutex> lk{msg_mutex_};
                    send_msg_cond_var_.wait(lk, [&]() {
                        return !send_msg_queue_.empty() || !this->msg_thread_start_;
                    });
                    while (!send_msg_queue_.empty()) {
                        auto &msg = send_msg_queue_.front();
                        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
                        f->push_back(msg.type_);
                        pm_tiny::fappend_value(*f, app_name);
                        session_->write_frame(f, 1);
                        send_msg_queue_.pop();
                    }
                }
                if (session_) {
                    session_->close();
                }
            } catch (const std::exception &ex) {
                fprintf(stderr, "%s:%s", __FUNCTION__, ex.what());
            }
        }

        static std::string getenv(const std::string &name) {
            auto c_env_val = ::getenv(name.c_str());
            if (c_env_val == nullptr) {
                return "";
            } else {
                return c_env_val;
            }
        }

        static std::string get_app_name_() {
            auto app_name = getenv(PM_TINY_APP_NAME);
            return app_name;
        }

        static std::string get_pm_tiny_addr_() {
            return getenv(PM_TINY_SOCK_FILE);
        }

        static bool get_pm_tiny_uds_abstract_namespace_() {
            return getenv(PM_TINY_UDS_ABSTRACT_NAMESPACE) == "1";
        }

        static std::unique_ptr<pm_tiny::session_t>
        make_session(const std::string &pm_tiny_addr,
                     bool uds_abstract_namespace,
                     long connect_timeout = 30) {
            struct sockaddr_un serun{};
            int len;
            int sockfd;
            const auto &sock_path = pm_tiny_addr;
            if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
                THROW_EXCEPTION_WITH_ERROR("socket");
            }
            memset(&serun, 0, sizeof(serun));
            serun.sun_family = AF_UNIX;
            if (uds_abstract_namespace) {
                serun.sun_path[0] = '\0';
                strncpy(serun.sun_path + 1, sock_path.c_str(), sizeof(serun.sun_path) - 2);
                len = offsetof(struct sockaddr_un, sun_path) + sock_path.length() + 1;
            } else {
                strncpy(serun.sun_path, sock_path.c_str(), sizeof(serun.sun_path) - 1);
                len = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
            }

#if PM_TINY_CLIENT_CONNECT_TIMEOUT
            int flags = fcntl(sockfd, F_GETFL, 0);
            if (flags < 0) {
                THROW_EXCEPTION_WITH_ERROR("fcntl");
            }
            if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                THROW_EXCEPTION_WITH_ERROR("fcntl");
            }
#endif
            if (connect(sockfd, (struct sockaddr *) &serun, len) < 0) {
                THROW_EXCEPTION_WITH_ERROR("connect");
            }
#if PM_TINY_CLIENT_CONNECT_TIMEOUT
            fd_set set;
            struct timeval timeout;
            FD_ZERO(&set); // 清除套接字集
            FD_SET(sockfd, &set);
            timeout.tv_sec = 0;
            timeout.tv_usec = connect_timeout*1000;
            int res = select(sockfd + 1, nullptr, &set, nullptr, &timeout);
            if (res < 0) {
                THROW_EXCEPTION_WITH_ERROR("select");
            } else if (res == 0) {
                THROW_EXCEPTION_WITH_ERROR("timeout");
            } else {
                // 检查是否真的连接上了
                int error;
                socklen_t len = sizeof(error);
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
                    THROW_EXCEPTION_WITH_ERROR("getsockopt");
                }
                if (error) {
                    THROW_EXCEPTION_WITH_ERROR("connect fail");
                }
            }
#endif
            auto session = std::make_unique<pm_tiny::session_t>(sockfd, 0);
            return session;
        }

    private:
        std::mutex msg_mutex_;
        std::string app_name_;
        std::string pm_tiny_addr_;
        bool uds_abstract_namespace_;
        std::unique_ptr<pm_tiny::session_t> session_;
        std::chrono::steady_clock::time_point time_point_;
        std::queue<Message> send_msg_queue_;
        size_t send_msg_queue_size = 1;
        std::thread msg_thread_;
        std::atomic_bool msg_thread_start_{};
        std::condition_variable send_msg_cond_var_;
    };

    AppClient::AppClient() : impl_(std::make_unique<AppClient::AppClientImpl>()) {
    }

    AppClient::~AppClient() = default;

    bool AppClient::is_enable() const {
        return impl_->is_enable();
    }

    std::string AppClient::get_app_name() const {
        return impl_->get_app_name();
    }

    void AppClient::tick() const {
        impl_->tick();
    }

    void AppClient::ready() const {
        impl_->ready();
    }
}


