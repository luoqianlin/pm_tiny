//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#include "prog.h"
#include "log.h"
#include <assert.h>
#include "pm_tiny_server.h"
#include "ANSI_color.h"

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

    void prog_info_t::write_prog_exit_message() {
        std::string msg_content;
        int msg_type;
        msg_type = 0;
        msg_content +=PM_TINY_ANSI_COLOR_REST "\n\n";
        msg_content += "PM_TINY MESSAGE:\n";
        msg_content += log_proc_exit_status(this, pid, last_wstatus);
        msg_content += "\n";
        this->write_msg_to_sessions(msg_type, msg_content);
    }
    /**
     * 监管的程序运行结束后会关闭pipefd,
     * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
     * */
    void prog_info_t::close_fds() {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1
                && this->logfile_fd[i] != -1) {
                read_pipe(i,1);
            }
        }
        write_prog_exit_message();
        this->close_pipefds();
        this->close_logfds();
        this->pid = -1;
    }

    std::string prog_info_t::get_dsc_name() const {
        return this->name + "(" + std::to_string(this->backup_pid) + ")";
    }

    void prog_info_t::init_prog_log() {
        cache_log.resize(0);
        residual_log = "";
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

    void prog_info_t::redirect_output_log(int i,std::string text){

        auto rotate_log_file = [this](int i) {
            close(this->logfile_fd[i]);
            pm_tiny::logger_t::logfile_cycle_write(this->logfile[i], this->logfile_count);
            int oflag = O_CREAT | O_RDWR | O_TRUNC;
            this->logfile_fd[i] = pm_tiny::f_log_open(this->logfile[i], oflag);
            this->logfile_size[i] = 0;
        };

        while (!text.empty()) {
            int off_out = this->logfile_size[i];
            if (off_out >= this->logfile_maxsize) {
//                logger->info("exceeds the maximum file size of %ld bytes,truncate\n",
//                             this->logfile_maxsize);
                rotate_log_file(i);
                off_out = 0;
            }
            int writeable_size = this->logfile_maxsize - off_out;
            writeable_size = std::min(writeable_size, (int) text.size());
            pm_tiny::safe_write(this->logfile_fd[i], text.c_str(), writeable_size);
            this->logfile_size[i] += writeable_size;
            if (writeable_size < text.size()) {
                text = text.substr(writeable_size);
            }else{
                break;
            }
        }
    }

    std::string prog_info_t::remove_ANSI_escape_code(const std::string& text){
        std::string output_text = this->residual_log + text;
        auto pair = mgr::utils::remove_ANSI_escape_code(output_text);
        this->residual_log = pair.second;
        auto pure_text = pair.first;
        return pure_text;
    }

    void prog_info_t::read_pipe(int i,int killed) {
        int nread;
        int rc;
        char buffer[4096];
        int &fd = this->rpipefd[i];
        do {
            ioctl(fd, FIONREAD, &nread);
            if (nread == 0) {
                close(fd);
                close(this->logfile_fd[i]);
                logger->debug("pid:%d pipe fd %d closed\n",
                              this->backup_pid, fd);
                fd = -1;
                this->logfile_fd[i] = -1;
                break;
            } else {
                std::string msg_content;
                int msg_type = 1;
                bool s_writeable = killed || this->is_sessions_writeable();
                if (!s_writeable) {
                    break;
                }
                int remaining_bytes = nread;
                do {
                    int max_nread;
                    max_nread = std::min(remaining_bytes, (int) sizeof(buffer));
                    assert(max_nread <= sizeof(buffer));
                    rc = (int) pm_tiny::safe_read(fd, buffer, max_nread);
                    if (rc > 0) {
                        std::string output_text(buffer, rc);
                        auto pure_text = remove_ANSI_escape_code(output_text);
                        redirect_output_log(i, pure_text);
#if PM_TINY_PTY_ENABLE
                        msg_content += output_text;
#endif
                        remaining_bytes -= rc;
                    } else if ((rc == -1 && errno != EINTR)) {
                        logger->syscall_errorlog("name:%s pid:%d fdin:%d fdout:%d read",
                                                 this->name.c_str(), this->pid, fd, this->logfile_fd[i]);
                        break;
                    }
                } while (remaining_bytes > 0);

#if PM_TINY_PTY_ENABLE
                    this->write_msg_to_sessions(msg_type, msg_content);
#endif
            }
        } while (killed);
    }

    void prog_info_t::write_cache_log_to_session(session_t *session) {
        if (!this->cache_log.empty()) {
            std::string prev_log(cache_log.data(), cache_log.size());
            auto new_frame = std::make_shared<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*new_frame, 1);
            pm_tiny::fappend_value(*new_frame, prev_log);
            session->write_frame(new_frame);
        }
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

        for (auto &session: sessions) {
            session->write_frame(wf);
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

    bool prog_info_t::is_kill_timeout() {
        return (pm_tiny::time::gettime_monotonic_ms() - request_stop_timepoint) >= kill_timeout_sec * 1000;;
    }

    void prog_info_t::async_force_kill() {
        assert(this->pid != -1);
        int rc = kill(this->pid, SIGKILL);
        if (rc != 0) {
            PM_TINY_LOG_E_SYS("kill");
        }
    }

    void prog_info_t::execute_penddingtasks(pm_tiny_server_t& pm_tiny_server) {
        if(!kill_pendingtasks.empty()) {
            for (auto &t: kill_pendingtasks) {
                t(pm_tiny_server);
            }
            kill_pendingtasks.clear();
        }
    }
    void prog_info_t::async_kill_prog() {
        assert(this->pid != -1);
        this->state = PM_TINY_PROG_STATE_REQUEST_STOP;
        this->request_stop_timepoint = pm_tiny::time::gettime_monotonic_ms();
        int rc = kill(this->pid, SIGTERM);
        if (rc == -1) {
            PM_TINY_LOG_E_SYS("kill");
        }
        alarm(this->kill_timeout_sec);
    }
}