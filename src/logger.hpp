#ifndef PM_TINY_LOGGER_HPP
#define PM_TINY_LOGGER_HPP

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <sys/time.h>
#include <limits.h>

#include "../src/time_util.h"
#include "../src/signal_util.h"
#include "pm_sys.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>


namespace pm_tiny {

    class logger_t {
    private:
        const char *COLOR_RED = "\033[31m";
        const char *COLOR_YELLOW = "\033[33m";
        const char *COLOR_GRAY = "\033[2m";
        const char *COLOR_WHITE = "\033[0m";
        const char *COLOR_WHITE_HIGHLIGHT = "\033[1m";
        const char *COLOR_GREEN = "\033[32m";

        int no_color = 0;
        bool is_tty = false;

        void init(int fp, long int f_maxsize = 4 * 1024 * 1024L) {

            // https://no-color.org/
            if (getenv("NO_COLOR") != nullptr) {
                no_color = 1;
            } else {
                no_color = 0;
            }

            this->fd_ = fp;
            this->f_maxsize_ = f_maxsize;
            this->is_tty = isatty(fp);
            if (!is_tty) {
                struct stat st;
                int rc = fstat(fp, &st);
                if (rc == -1) {
                    perror("fstat");
                }
                this->f_size_ = st.st_size;
            }
        }

    public:
        enum class log_level_t {
            debug, info, error
        };

        explicit logger_t(const char *logfile_path,
                          long int f_maxsize = 4 * 1024 * 1024L,
                          int out_stdout = 1, int log_file_count = 3) {
            auto fp = open(logfile_path, O_APPEND | O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
            this->out_stdout_ = out_stdout;
            this->f_path_ = logfile_path;
            this->log_file_count_ = log_file_count;
            this->init(fp, f_maxsize);
        }

        explicit logger_t(int fd = STDOUT_FILENO) noexcept {
            this->f_path_ = "";
            this->out_stdout_ = 0;
            this->init(STDOUT_FILENO);
        }

        logger_t(const logger_t &) = delete;

        logger_t(logger_t &&) = delete;

        logger_t &operator=(const logger_t &) = delete;

        ~logger_t() {
            if (!f_path_.empty()) {
                close(this->fd_);
                this->fd_ = -1;
            }
        }


        char *vmake_message(const char *fmt, va_list ap) {
            int size = 0;
            char *p = nullptr;
            va_list p2;
            va_copy(p2, ap);
            /* Determine required size */
            size = vsnprintf(p, size, fmt, p2);
            va_end(p2);

            if (size < 0)
                return nullptr;

            size++;             /* For '\0' */
            p = (char *) malloc(size);
            if (p == nullptr)
                return nullptr;
            size = vsnprintf(p, size, fmt, ap);
            if (size < 0) {
                free(p);
                return nullptr;
            }

            return p;
        }

        char *make_message(const char *fmt, ...) {
            char *p = nullptr;
            va_list ap;
            va_start(ap, fmt);
            p = vmake_message(fmt, ap);
            va_end(ap);
            return p;
        }

        std::string log_level_to_str(log_level_t level) {
            std::string level_str = "";
            switch (level) {
                case log_level_t::debug:
                    level_str = "debug";
                    break;
                case log_level_t::info:
                    level_str = "info";
                    break;
                case log_level_t::error:
                    level_str = "error";
                    break;
            }
            struct timeval tvc;
            time_t tval;
            char buf[sizeof("yyyy-mm-dd hh:mm:ss") + /*paranoia:*/ 4];
            gettimeofday(&tvc, nullptr);/* never fails */
            tval = tvc.tv_sec;
            pm_tiny::time::strftime_YYYYMMDDHHMMSS(buf, sizeof(buf), &tval);
            char buf2[sizeof(buf) * 2];
            sprintf(buf2, "%s.%06u [%5s] ", buf, (unsigned) tvc.tv_usec, level_str.c_str());
            return buf2;

        }

        /*
         *
         * */
        static void logfile_cycle_write(const std::string &f_path, int log_file_count,
                                        std::string logfileext = ".log") {
            if (log_file_count < 1)return;
            std::string logfilename = f_path;
            auto dot_idx = f_path.rfind('.');
            if (dot_idx != std::string::npos) {
                logfilename = f_path.substr(0, dot_idx);
                logfileext = f_path.substr(dot_idx);
            }
            bool find_emtpy = false;
            for (int i = 0; i < log_file_count; i++) {
                std::string new_log_file = logfilename;
                new_log_file += ("_" + std::to_string(i) + logfileext);
                if (access(new_log_file.c_str(), F_OK) != 0) {
                    rename(f_path.c_str(), new_log_file.c_str());
                    find_emtpy = true;
                    break;
                }
            }
            if (!find_emtpy) {
                std::string log_file = logfilename;
                log_file += ("_" + std::to_string(0) + logfileext);
                unlink(log_file.c_str());
                for (int i = 1; i < log_file_count; i++) {
                    std::string cur_log_file = logfilename;
                    cur_log_file += ("_" + std::to_string(i) + logfileext);

                    std::string prev_log_file = logfilename;
                    prev_log_file += ("_" + std::to_string(i - 1) + logfileext);
                    rename(cur_log_file.c_str(), prev_log_file.c_str());
                }
                std::string last_log_file = logfilename;
                last_log_file += ("_" + std::to_string(log_file_count - 1) + logfileext);
                rename(f_path.c_str(), last_log_file.c_str());
            }
        }

        void check_rotate_log_file(const char *content, int len) {
            if (f_size_ != -1 && f_maxsize_ > 0) {
                int remin_bytes = len;
                if (f_size_ < f_maxsize_) {
                    int write_bytes = std::min(static_cast<int>(f_maxsize_ - f_size_), len);
                    remin_bytes = len - write_bytes;
                    if (write_bytes > 0) {
                        safe_write(fd_, content, write_bytes);
                        content += write_bytes;
                        f_size_ += write_bytes;
                    }
                }
                if (remin_bytes > 0) {
                    close(this->fd_);
                    logfile_cycle_write(this->f_path_, this->log_file_count_);
                    this->fd_ = open(this->f_path_.c_str(), O_TRUNC | O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
                    this->f_size_ = remin_bytes;
                    safe_write(fd_, content, remin_bytes);
//                    printf("exceeds the maximum file size of %ld bytes,truncate\n",
//                           f_maxsize_);
                }
            }
        }

        static void write_log_with_color(int fd, const char *content, int len, const char *color) {
            const char *reset = "\033[0m";
            safe_write(fd, color, strlen(color));
            safe_write(fd, content, len);
            safe_write(fd, reset, strlen(reset));
        }

        void check_size_write_log(const char *content, int len, const char *color) {
            if (!is_tty) {
                check_rotate_log_file(content, len);
                if (this->out_stdout_) {
                    write_log_with_color(STDOUT_FILENO, content, len, color);
                }
            } else {
                write_log_with_color(this->fd_, content, len, color);
            }
        }

        void vlog(const log_level_t log_level,
                  const char *format, va_list p) {
            char *fmt_msg = nullptr;
            fmt_msg = vmake_message(format, p);
            std::string log_prefix = log_level_to_str(log_level);
            std::string log_content(fmt_msg);
            free(fmt_msg);
            log_content = log_prefix + log_content;
            if (log_content[log_content.length() - 1] != '\n') {
                log_content += '\n';
            }
            int len = (int) log_content.length();
            const char *_log_content = log_content.c_str();

            const char *color = COLOR_GRAY;
            switch (log_level) {
                case log_level_t::debug:
                    color = COLOR_GRAY;
                    break;
                case log_level_t::info:
                    color = COLOR_WHITE;
                    break;
                case log_level_t::error:
                    color = COLOR_RED;
                    break;
            }
            check_size_write_log(_log_content, len, color);
        }


        void log(const log_level_t log_level, const char *format, ...) {
            va_list p;
            va_start(p, format);
            vlog(log_level, format, p);
            va_end(p);

        }

        void info(const char *format, ...) {
            va_list p;
            va_start(p, format);
            vlog(log_level_t::info, format, p);
            va_end(p);
        }

        void debug(const char *format, ...) {
            va_list p;
            va_start(p, format);
            vlog(log_level_t::debug, format, p);
            va_end(p);
        }

        void error(const char *format, ...) {
            va_list p;
            va_start(p, format);
            vlog(log_level_t::error, format, p);
            va_end(p);
        }

        void safe_signal_log(int sig) {
            char buf[200] = {0};
            mgr::utils::signal::signal_log(sig, buf);
            check_size_write_log(buf, strlen(buf),COLOR_WHITE_HIGHLIGHT);
        }

        void syscall_errorlog(const char *format, ...) {
            char *error_msg = errno ? strerror(errno) : nullptr;
            std::string _format(format);
            if (error_msg) {
                _format = _format + ": " + error_msg;
            }
            va_list p;
            va_start(p, format);
            vlog(log_level_t::error, _format.c_str(), p);
            va_end(p);
        }

        void safe_syscall_errorlog(const char *msg) {
            const char *error_msg = errno ? strerror(errno) : nullptr;
            int msg_max_len = 30;
            char buf[400] = {0};
            int buf_max_len = sizeof(buf) / sizeof(buf[0]);
            strncat(buf, msg, msg_max_len);
            if (error_msg) {
                strcat(buf, ": ");
                strncat(buf, error_msg, buf_max_len - msg_max_len - 3);
            }
            int len = strlen(buf);
            if (len < buf_max_len - 1) {
                if (buf[len] != '\n') {
                    buf[len] = '\n';
                    buf[len + 1] = 0;
                }
            } else if (buf[buf_max_len - 2] != '\n') {
                buf[buf_max_len - 2] = '\n';
                buf[buf_max_len - 1] = 0;
            }
            check_size_write_log(buf, strlen(buf),COLOR_RED);
        }


    private:
        int fd_;
        long int f_size_ = -1;
        long int f_maxsize_ = 4 * 1024 * 1024; //4M
        std::string f_path_;
        int out_stdout_ = 0;
        int log_file_count_ = 3;
    };

}
#endif//PM_TINY_LOGGER_HPP