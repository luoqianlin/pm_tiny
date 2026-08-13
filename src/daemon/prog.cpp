
#include "prog.h"
#include "log.h"
#include <assert.h>
#include "pm_tiny_server.h"
#include "ANSI_color.h"
#include "unordered_map"
#include "android_lmkd.h"


namespace pm_tiny {
    auto f_close(int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };

    auto f_log_open(std::string &path, int oflag = O_CREAT | O_RDWR) {
        const char *file = path.c_str();
        int fd = open(file, oflag | O_CLOEXEC, S_IRUSR | S_IWUSR);
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
        os << "name:'" << prog.name + "'" << " pid:" << prog.instance.pid << " ";
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
        auto prog_pid = instance.pid;
        if (prog_pid == -1) {
            prog_pid = instance.last_pid;
        }
        std::string msg_content;
        int msg_type;
        msg_type = 0;
        msg_content += PM_TINY_ANSI_COLOR_REST "\n\n";
        msg_content += "PM_TINY MESSAGE:\n";
        msg_content += log_proc_exit_status(this, prog_pid, last_wstatus);
        msg_content += "\n";
        this->write_msg_to_sessions(msg_type, msg_content);
        for (auto &session: this->sessions) {
            session->mark_close();
        }
        this->sessions.clear();
    }

    /**
     * 监管的程序运行结束后会关闭pipefd,
     * 事件循环会监听到 pipefd 关闭，进而关闭 pipefd 和对应的日志文件 fd。
     * */
    void prog_info_t::close_fds(const CloseableFd& lmkd) {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1
                && this->logfile_fd[i] != -1) {
                read_pipe(i, 1);
            }
        }
        if (this->instance.pid != -1 && lmkd) {
            cmd_procremove(lmkd.fd_,this->instance.pid);
        }
        write_prog_exit_message();
        this->close_pipefds();
        this->close_logfds();
        this->instance.pid = -1;
    }

    std::string prog_info_t::get_desc_name() const {
        return this->name + "(" + std::to_string(this->instance.last_pid) + ")";
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

    void prog_info_t::redirect_output_log(int i, std::string text) {

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
            if (writeable_size < static_cast<int>(text.size())) {
                text = text.substr(writeable_size);
            } else {
                break;
            }
        }
    }

    std::string prog_info_t::remove_ANSI_escape_code(const std::string &text) {
        std::string output_text = this->residual_log + text;
        auto pair = mgr::utils::remove_ANSI_escape_code(output_text);
        this->residual_log = pair.second;
        auto pure_text = pair.first;
        return pure_text;
    }

    void prog_info_t::read_pipe(int i, int killed) {
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
                              this->instance.last_pid, fd);
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
                    assert(max_nread <= static_cast<int>(sizeof(buffer)));
                    rc = (int) pm_tiny::safe_read(fd, buffer, max_nread);
                    if (rc > 0) {
                        std::string output_text(buffer, rc);
                        std::string pure_text;
                        if (this->use_pty) {
                            pure_text = remove_ANSI_escape_code(output_text);
                        } else {
                            pure_text = output_text;
                        }
                        redirect_output_log(i, pure_text);
                        if (this->use_pty) {
                            msg_content += output_text;
                        }
                        remaining_bytes -= rc;
                    } else if ((rc == -1 && errno != EINTR)) {
                        logger->syscall_errorlog("name:%s pid:%d fdin:%d fdout:%d read",
                                                 this->name.c_str(), this->instance.pid, fd, this->logfile_fd[i]);
                        break;
                    }
                } while (remaining_bytes > 0);

                if (this->use_pty) {
                    this->write_msg_to_sessions(msg_type, msg_content);
                }
            }
        } while (killed);
    }

    void prog_info_t::write_cache_log_to_session(session_t *session) {
        if (!this->cache_log.empty()) {
            std::string prev_log(cache_log.data(), cache_log.size());
            auto frames = str_to_frames(1, prev_log);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                session->write_stream_frame(frames[i], 0, i + 1 < frames.size());
            }
        }
    }

    void prog_info_t::write_msg_to_sessions(int msg_type, const std::string &msg_content) {
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
        if (!this->sessions.empty()) {
            auto frames = str_to_frames(msg_type, msg_content);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                for (auto &session: sessions) {
                    session->write_stream_frame(frames[i], 0, i + 1 < frames.size());
                }
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
        std::string app_state = "unkown";
        if (prog) {
            prog_name = (char *) prog->name.c_str();
            run_time = (float) (pm_tiny::time::gettime_monotonic_ms() - prog->last_startup_ms) / (60 * 1000.0f);
            restart_count = prog->dead_count;
            app_state = pm_state_to_str(prog->state);
        }
        char s_buff[1024];
        if (WIFEXITED(wstatus)) {
            int exit_code = WEXITSTATUS(wstatus);
            snprintf(s_buff, sizeof(s_buff),
                     "`%s`(%d) exited, exit code=%d run time=%.3fmin restart=%d state=%s\n",
                     prog_name, pid, exit_code, run_time, restart_count, app_state.c_str());
        } else if (WIFSIGNALED(wstatus)) {
            int kill_signo = WTERMSIG(wstatus);
            char buf[80] = {0};
            mgr::utils::signal::signo_to_str(kill_signo, buf, false);
            snprintf(s_buff, sizeof(s_buff),
                     "`%s`(%d) killed by signal %s run time=%.3fmin restart=%d state=%s\n",
                     prog_name, pid, buf, run_time, restart_count, app_state.c_str());
        }
        return s_buff;
    }

    bool prog_info_t::is_start_timeout() const {
        auto t = start_timeout;
        if (t < 0)return false;
        return (pm_tiny::time::gettime_monotonic_ms() - last_startup_ms) >= t * 1000;
    }

    bool prog_info_t::is_tick_timeout() const {
        if (heartbeat_timeout <= 0)return false;
        return time::gettime_monotonic_ms() - last_tick_timepoint >= heartbeat_timeout * 1000;
    }

    void prog_info_t::async_force_kill() {
        if (instance.job.force(instance.generation) == termination_action::send_kill) {
            instance.termination = instance.job.phase();
            signal_tree(SIGKILL);
        }
    }

    termination_action prog_info_t::poll_termination() {
        auto action = instance.job.poll(instance.generation, time::gettime_monotonic_ms(), is_tree_empty());
        instance.termination = instance.job.phase();
        return action;
    }

    termination_action prog_info_t::mark_tree_draining() {
        auto action = instance.job.mark_tree_draining(instance.generation,
                                                       time::gettime_monotonic_ms(),
                                                       kill_timeout_sec);
        instance.termination = instance.job.phase();
        return action;
    }

    void prog_info_t::signal_tree(int signo) {
        int rc = tree_controller ? tree_controller->signal(instance.tree, signo)
                                 : (instance.pid != -1 ? kill(instance.pid, signo) : 0);
        if (rc != 0) {
            PM_TINY_LOG_E_SYS("kill");
        }
    }

    void prog_info_t::execute_penddingtasks(pm_tiny_server_t &pm_tiny_server) {
        if (!kill_pendingtasks.empty()) {
            const uint64_t completed_generation = instance.generation;
            for (auto &pending: kill_pendingtasks) {
                if (!pending.matches(completed_generation) ||
                    instance.generation != completed_generation) continue;
                pending.task(pm_tiny_server);
            }
            kill_pendingtasks.clear();
        }
    }

    void prog_info_t::enqueue_after_termination(task_fun_t task) {
        kill_pendingtasks.push_back({instance.generation, std::move(task)});
    }

    void prog_info_t::async_kill_prog() {
        auto action = instance.job.request(instance.generation, time::gettime_monotonic_ms(), kill_timeout_sec);
        instance.termination = instance.job.phase();
        this->state = PM_TINY_PROG_STATE_REQUEST_STOP;
        if (action == termination_action::send_term) signal_tree(SIGTERM);
//      alarm(this->kill_timeout_sec);
    }

    bool prog_info_t::is_tree_empty() const {
        return tree_controller ? tree_controller->empty(instance.tree) : instance.pid == -1;
    }

    int64_t prog_info_t::update_count_timer() {
        auto now_ms = pm_tiny::time::gettime_monotonic_ms();
        if ((now_ms - this->last_dead_time_ms) > this->moniter_duration_threshold) {
            this->last_dead_time_ms = now_ms;
            this->dead_count_timer = 0;
        } else {
            this->dead_count_timer++;
        }
        return now_ms;
    }

    bool prog_info_t::is_reach_max_num_death() {
        auto not_max_dead_count = (this->dead_count_timer < this->moniter_duration_max_dead_count ||
                                   this->moniter_duration_max_dead_count <= 0);
        return !not_max_dead_count;
    }

    restart_decision prog_info_t::plan_automatic_restart(int64_t now_ms, int64_t runtime_ms) {
        auto decision = pm_tiny::plan_automatic_restart(restart_config, restart_state, now_ms, runtime_ms);
        restart_pending = decision.restart;
        restart_due_ms = decision.restart ? now_ms + decision.delay_ms : 0;
        return decision;
    }

    void prog_info_t::reset_restart_policy() {
        restart_state.reset();
        restart_pending = false;
        restart_due_ms = 0;
    }

    void prog_info_t::detach_sessions() {
        for (auto session: this->sessions) {
            session->set_prog(nullptr);
        }
        this->sessions.clear();
    }

    bool prog_info_t::is_cfg_equal(const prog_ptr_t prog) const {
        auto old_args = this->args;
        auto old_deps = this->depends_on;
        auto new_args = prog->args;
        auto new_deps = prog->depends_on;

        std::sort(old_args.begin(), old_args.end());
        std::sort(new_args.begin(), new_args.end());
        std::sort(old_deps.begin(), old_deps.end());
        std::sort(new_deps.begin(), new_deps.end());

        return old_args == new_args
               && old_deps == new_deps
               && this->work_dir == prog->work_dir;
    }

    bool build_prog_dependency_graph(const std::vector<prog_ptr_t> &progs,
                                     dependency_graph &graph,
                                     std::string &error_message) {
        std::vector<dependency_node_config> configs;
        configs.reserve(progs.size());
        for (const auto prog : progs) configs.push_back({prog->name, prog->depends_on});
        dependency_error error;
        if (!dependency_graph::build(configs, graph, error)) {
            error_message = error.message;
            return false;
        }
        error_message.clear();
        return true;
    }
}
