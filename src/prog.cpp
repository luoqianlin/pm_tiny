//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#include "prog.h"
#include "log.h"
#include <assert.h>

namespace pm_tiny {
    auto f_close(int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };

    auto f_log_open(std::string &path, int oflag = O_CREAT | O_RDWR) {
        const char *file = path.c_str();
        int fd = open(file, oflag, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            pm_tiny::logger->syscall_errorlog("open");
        }
        return fd;
    };

    auto get_file_size(int fd) {
        struct stat st;
        int rc = fstat(fd, &st);
        if (rc == -1) {
            logger->syscall_errorlog("fstat");
        }
        return st.st_size;
    };


    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog) {
        os << "name:'" << prog.name + "'" << " pid:" << prog.pid << " ";
        os << "work dir:" << prog.work_dir << " ";
        os << "args: ";
        std::for_each(std::begin(prog.args), std::end(prog.args), [&os](const std::string &s) {
            os << s << " ";
        });
        return os;
    }

    void prog_info_t::close_pipefds() {
        std::for_each(std::begin(this->rpipefd),
                      std::end(this->rpipefd),
                      pm_tiny::f_close);
    }

    void prog_info_t::close_logfds() {
        std::for_each(std::begin(this->logfile_fd),
                      std::end(this->logfile_fd),
                      pm_tiny::f_close);
    }

    void prog_info_t::set_state(int s) {
        this->state = s;
    }

    /**
     * 监管的程序运行结束后会关闭pipefd,
     * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
     * */
    void prog_info_t::close_fds() {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1
                && this->logfile_fd[i] != -1) {
//                    logger->debug("start safe close fds");
                read_pipe(i);
//                    logger->debug("end safe close fds");
            }
        }
        this->close_pipefds();
        this->close_logfds();
        this->pid = -1;
    }

    std::string prog_info_t::get_dsc_name() const {
        return this->name + "(" + std::to_string(this->backup_pid) + ")";
    }

    void prog_info_t::init_prog_log() {
        cache_log.resize(0);
        for (int i = 0; i < 2; i++) {
            if (this->logfile[i].empty())continue;
            int oflag = O_CREAT | O_RDWR;
            this->logfile_fd[i] = f_log_open(this->logfile[i], oflag);
            lseek64(this->logfile_fd[i], 0, SEEK_END);
            this->logfile_size[i] = get_file_size(this->logfile_fd[i]);
            logger->info("log file %s  %ld bytes\n",
                         this->logfile[i].c_str(), this->logfile_size[i]);
        }
    }

    void prog_info_t::read_pipe(int i) {
        int nread;
        int rc;
        char buffer[4096];
        int &fd = this->rpipefd[i];
        ioctl(fd, FIONREAD, &nread);
//            printf("FIONREAD:%d\n",nread);
        std::string msg_content;
        int msg_type;
        bool s_writeable = this->is_sessions_writeable();
        if (nread == 0) {
            close(fd);
            close(this->logfile_fd[i]);
            logger->debug("pid:%d pipe fd %d closed\n",
                          this->backup_pid, fd);
            fd = -1;
            this->logfile_fd[i] = -1;
            msg_type = 0;
            msg_content = log_proc_exit_status(this, pid, last_wstatus);
            msg_content += "\n";
            s_writeable = true;
        } else {
            msg_type = 1;
            int remaining_bytes = nread;
            do {
                int max_nread;

                if (!s_writeable) {
                    break;
                }
#if PM_TINY_PIPE_SPLICE
                max_nread = remaining_bytes;
#else
                max_nread = std::min(remaining_bytes, (int) sizeof(buffer));
#endif

//                    off64_t off_out;
//                    off_out = lseek64(this->logfile_fd[i], 0, SEEK_END);
//                    if (off_out == -1) {
//                        logger->syscall_errorlog("name:%s pid:%d lseek64", this->name.c_str(), this->pid);
//                        break;
//                    }
                auto rotate_log_file = [this](int i) {
                    close(this->logfile_fd[i]);
                    pm_tiny::logger_t::logfile_cycle_write(this->logfile[i], this->logfile_count);
                    int oflag = O_CREAT | O_RDWR | O_TRUNC;
                    this->logfile_fd[i] = pm_tiny::f_log_open(this->logfile[i], oflag);
                    this->logfile_size[i] = 0;
                };
                int off_out = this->logfile_size[i];
                if (off_out >= this->logfile_maxsize) {
//                        logger->info("exceeds the maximum file size of %ld bytes,truncate\n",
//                                     this->logfile_maxsize);
                    rotate_log_file(i);
                    off_out = 0;
                }
                int exceed_n = off_out + remaining_bytes - this->logfile_maxsize;

                if (exceed_n > 0) {
                    max_nread = std::min(int(this->logfile_maxsize - off_out), max_nread);
                }
#if PM_TINY_PIPE_SPLICE
                    do {
                        rc = splice(fd, nullptr, this->logfile_fd[i],
                                    nullptr, (size_t) max_nread, 0);
                    } while (rc == -1 && errno == EAGAIN);
#else
                assert(max_nread <= sizeof(buffer));
                rc = (int) pm_tiny::safe_read(fd, buffer, max_nread);
#endif
                if (rc > 0) {
#if !PM_TINY_PIPE_SPLICE
                    pm_tiny::safe_write(this->logfile_fd[i], buffer, rc);
#if PM_TINY_PTY_ENABLE
                    msg_content += std::string(buffer, rc);
#endif
#endif
                    this->logfile_size[i] += rc;
                    remaining_bytes -= rc;
                } else if ((rc == -1 && errno != EINTR)) {
                    logger->syscall_errorlog("name:%s pid:%d fdin:%d fdout:%d off_out:%d splice",
                                             this->name.c_str(), this->pid, fd, this->logfile_fd[i], (int) off_out);
                    break;
                }
            } while (remaining_bytes > 0);
        }

#if PM_TINY_PTY_ENABLE
        if (s_writeable) {
            this->write_msg_to_sessions(msg_type, msg_content);
        }
#endif
    }

    void prog_info_t::write_msg_to_sessions(int msg_type, std::string &msg_content) {
        auto wf = std::make_shared<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*wf, msg_type);
        pm_tiny::fappend_value(*wf, msg_content);
        int cur_cache_log_size = (int) cache_log.size();
        int new_msg_len = (int) msg_content.size();
        int total = cur_cache_log_size + new_msg_len;
        int remain = MAX_CACHE_LOG_LEN - total;
        if (remain >= 0) {
            std::copy(msg_content.begin(), msg_content.end(),
                      std::back_inserter(cache_log));
        } else {
            int move_out = -remain;
            if (move_out < cur_cache_log_size) {
                int N = cur_cache_log_size - move_out;
                for (int i = 0; i < N; i++) {
                    cache_log[i] = cache_log[i + move_out];
                }
                for (int i = N; i < cur_cache_log_size; i++) {
                    cache_log[i] = msg_content[i - N];
                }
                std::copy(msg_content.begin() + move_out, msg_content.end(),
                          std::back_inserter(cache_log));
            } else {
                cache_log.resize(MAX_CACHE_LOG_LEN);
                std::copy(msg_content.begin() + (move_out - cur_cache_log_size), msg_content.end(),
                          cache_log.begin());
            }
        }
        std::string prev_log(cache_log.data(), cache_log.size());
        auto new_frame = std::make_shared<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*new_frame, msg_type);
        pm_tiny::fappend_value(*new_frame, prev_log);

        for (auto &session: sessions) {
            if (session->is_new_created_) {
                session->write_frame(new_frame);
                session->is_new_created_ = false;
            } else {
                session->write_frame(wf);
            }
        }
    }

    bool prog_info_t::remove_session(session_t *session) {
        sessions.erase(std::remove(sessions.begin(), sessions.end(), session),
                       sessions.end());
        return true;
    }

    void prog_info_t::add_session(session_t *session) {
        this->sessions.emplace_back(session);
        session->set_prog(this);
    }

    bool prog_info_t::is_sessions_writeable() {
        for (auto &session: this->sessions) {
            if (session->sbuf_size() > 0) {
                return false;
            }
        }
        return true;
    }

    std::string prog_info_t::log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus) {
        const char *prog_name = "Unkown";
        float run_time = NAN;
        int restart_count = 0;
        if (prog) {
            prog_name = (char *) prog->name.c_str();
            run_time = (float) (pm_tiny::time::gettime_monotonic_ms() - prog->last_startup_ms) / (60 * 1000.0f);
            restart_count = prog->dead_count;
        }
        char s_buff[1024];
        if (WIFEXITED(wstatus)) {
            int exit_code = WEXITSTATUS(wstatus);
            snprintf(s_buff, sizeof(s_buff), "pid:%d name:%s exited, exit code %d run time:%.3f minutes restart:%d\n",
                     pid, prog_name, exit_code, run_time, restart_count);
        } else if (WIFSIGNALED(wstatus)) {
            int kill_signo = WTERMSIG(wstatus);
            char buf[80] = {0};
            mgr::utils::signal::signo_to_str(kill_signo, buf, false);
            snprintf(s_buff, sizeof(s_buff), "pid:%d name:%s killed by signal %s run time:%.3f minutes restart:%d\n",
                     pid, prog_name, buf, run_time, restart_count);
        }
        return s_buff;
    }
}