//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

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


    std::ostream &operator<<(std::ostream &os, const prog_info_wrapper_t &prog_info) {
        return os << (prog_info.prog_info != nullptr ? prog_info.prog_info->name : "ROOT");
    }

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
        auto prog_pid = pid;
        if (prog_pid == -1) {
            prog_pid = backup_pid;
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
     * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
     * */
    void prog_info_t::close_fds(const CloseableFd& lmkd) {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1
                && this->logfile_fd[i] != -1) {
                read_pipe(i, 1);
            }
        }
        if (this->pid != -1 && lmkd) {
            cmd_procremove(lmkd.fd_,this->pid);
        }
        write_prog_exit_message();
        this->close_pipefds();
        this->close_logfds();
        this->pid = -1;
    }

    std::string prog_info_t::get_desc_name() const {
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
                    assert(max_nread <= static_cast<int>(sizeof(buffer)));
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
            auto frames = str_to_frames(1, prev_log);
            for (auto &wf: frames) {
                session->write_frame(wf);
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
            for (auto &wf: frames) {
                for (auto &session: sessions) {
                    session->write_frame(wf);
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

    bool prog_info_t::is_kill_timeout() const {
        return (pm_tiny::time::gettime_monotonic_ms() - request_stop_timepoint) >= kill_timeout_sec * 1000;;
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
        assert(this->pid != -1);
        int rc = kill(this->pid, SIGKILL);
        if (rc != 0) {
            PM_TINY_LOG_E_SYS("kill");
        }
    }

    void prog_info_t::execute_penddingtasks(pm_tiny_server_t &pm_tiny_server) {
        if (!kill_pendingtasks.empty()) {
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
//      alarm(this->kill_timeout_sec);
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

    int get_min_start_timeout(const proglist_t &start_progs) {
        int alarm_time = 1000;
        for (const auto &p: start_progs) {
            auto t = p->start_timeout;
            if (t <= 0) {
                t = 3;
            }
            alarm_time = std::min(alarm_time, t);
        }
        return alarm_time;
    }

    std::unique_ptr<ProgDAG>
    check_prog_info(const std::vector<prog_ptr_t> &prog_cfgs) {
        std::unordered_map<std::string, int> name2idx;
        for (int i = 0; i < static_cast<int>(prog_cfgs.size()); i++) {
            auto &name = prog_cfgs[i]->name;
            if (name2idx.count(name) > 0) {
                PM_TINY_LOG_E("name %s already exists ignore", name.c_str());
                return nullptr;
            }
            name2idx[name] = i;
        }
        int vertex_count = static_cast<int>(name2idx.size() + 1);
        auto graph = std::make_unique<prog_graph_t>(vertex_count);
        for (int i = 0; i < static_cast<int>(prog_cfgs.size()); i++) {
            const auto &cfg = prog_cfgs[i];
            if (cfg->depends_on.empty()) {
                graph->add_edge(0, i + 1);
            } else {
                for (auto &sc: cfg->depends_on) {
                    if (name2idx.count(sc) > 0) {
                        graph->add_edge(name2idx[sc] + 1, i + 1);
                    } else {
                        PM_TINY_LOG_E("depends_on: %s not found", sc.c_str());
                        return nullptr;
                    }
                }
            }
            graph->vertex(i + 1).prog_info = const_cast<prog_info_t *>(cfg);
        }
        graph->vertex(0).prog_info = nullptr;
        auto no_cycle = graph->topological_sort();
        if (!no_cycle) {
            PM_TINY_LOG_E("The dependency configuration is invalid, there is a cycle in the directed graph");
            return nullptr;
        }
        auto dag = std::make_unique<ProgDAG>(std::move(graph));
        return dag;
    }

    ProgDAG::ProgDAG(std::unique_ptr<prog_graph_t> graph) {
        this->graph = std::move(graph);
    }

    proglist_t ProgDAG::start() {
        proglist_t start_progs;
        std::vector<int> start_nodes;
        show_in_degree_info();
        graph->remove_vertex(0);
        for (size_t i = 0; i < graph->vertex_count(); ++i) {
            if (this->graph->in_degree(i) == 0) {
                start_progs.push_back(this->graph->vertex(i).prog_info);
            }
        }
        show_in_degree_info();
        return start_progs;
    }

    void ProgDAG::remove(const proglist_t &pl) {
        if (graph->vertex_count() == 0)return;
        std::vector<prog_graph_t::vertex_index_t> graph_vertices;
        for (const auto &p: pl) {
            auto vertex_i = graph->vertex_index([&p](auto &p_info) {
                return p == p_info.prog_info;
            });
            if (vertex_i != prog_graph_t::npos) {
                graph_vertices.push_back(vertex_i);
            }
        }
        std::sort(graph_vertices.begin(), graph_vertices.end(), std::greater<>());
        for (auto vertex: graph_vertices) {
            this->graph->remove_vertex(vertex);
        }
    }

    proglist_t ProgDAG::next(const proglist_t &pl) {
        proglist_t start_progs;
        this->remove(pl);
        for (size_t i = 0; i < graph->vertex_count(); ++i) {
            if (this->graph->in_degree(i) == 0) {
                start_progs.push_back(this->graph->vertex(i).prog_info);
            }
        }
        show_in_degree_info();
        return start_progs;
    }

    bool ProgDAG::is_traversal_complete() const {
        return this->graph->vertex_count() == 0;
    }

    void ProgDAG::show_in_degree_info() const {
        auto graph_info = this->graph->dump_vertex_in_out_degree();
        PM_TINY_LOG_D("graph info:%s", graph_info.c_str());
    }

    void ProgDAG::show_depends_info() const {
        std::stringstream ss;
        for (prog_graph_t::vertex_index_t i = 1; i < graph->vertex_count(); i++) {
            auto vertices = graph->in_vertices(i);
            ss << graph->vertex(i) << " : ";
            for (auto iter = vertices.cbegin(); iter != vertices.cend(); iter++) {
                auto v = *iter;
                if (v == 0)continue;
                ss << graph->vertex(v);
                if (std::next(iter) != vertices.cend()) {
                    ss << ",";
                }
            }
            ss << std::endl;
        }
        PM_TINY_LOG_I("depends:\n%s", ss.str().c_str());
    }
}