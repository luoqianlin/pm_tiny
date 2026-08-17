//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#include "prog.h"
#include "daemon_log.h"
#include "signal_util.h"
#include <assert.h>
#include "pm_tiny_server.h"
#include "ANSI_color.h"
#include "unordered_map"
#include "android_lmkd.h"
#include "time_util.h"


namespace pm_tiny {
    auto f_close(int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };

    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog) {
        os << "name:'" << prog.name + "'" << " pid:" << prog.instance.pid << " ";
        os << "work dir:" << prog.work_dir << " ";
        os << "executable: " << prog.executable << " args: ";
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
        for (auto &writer : log_writers) writer.reset();
    }

    void prog_info_t::set_state(int s) {
        this->state = s;
    }

    void prog_info_t::write_prog_exit_message() {
        auto prog_pid = instance.pid;
        if (prog_pid == -1) {
            prog_pid = instance.last_pid;
        }
        std::string reason = "unknown";
        int code = 0;
        if (WIFEXITED(last_wstatus)) {
            reason = "exited";
            code = WEXITSTATUS(last_wstatus);
        } else if (WIFSIGNALED(last_wstatus)) {
            reason = "signaled";
            code = WTERMSIG(last_wstatus);
        }
        const auto msg_content = format_program_exit_event(name, prog_pid, reason, code);
        const int msg_type = 0;
        if (!sessions.empty()) {
            auto frames = str_to_frames(msg_type, msg_content);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                for (auto *session : sessions)
                    session->write_stream_frame(frames[i], 0, i + 1 < frames.size());
            }
        }
        detach_sessions();
    }

    /**
     * 监管的程序运行结束后会关闭pipefd,
     * 事件循环会监听到 pipefd 关闭，进而关闭 pipefd 和对应的日志文件 fd。
     * */
    void prog_info_t::close_fds(const CloseableFd& lmkd) {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1) {
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
        log_tail.clear();
        log_health.reset();
        residual_log = "";
        const int writer_count = log_mode == log_mode_t::combined ? 1 : 2;
        for (int i = 0; i < writer_count; i++) {
            if (this->logfile[i].empty())continue;
            log_writers[i].reset(new rotating_log_writer(
                logfile[i], static_cast<std::size_t>(log_max_size_kb) * 1024U, log_archive_count));
            std::string error;
            if (!log_writers[i]->open(error)) {
                log_health.record_failure(time::gettime_monotonic_ms(), 0, error);
                PM_TINY_DLOG_ERROR("program %s log degraded: %s", name.c_str(), error.c_str());
                log_writers[i].reset();
            }
        }
    }

    void prog_info_t::redirect_output_log(int i, std::string text) {
        const int writer_index = log_mode == log_mode_t::combined ? 0 : i;
        const auto now = time::gettime_monotonic_ms();
        if (!log_writers[writer_index] && (!log_health.degraded || log_health.retry_ready(now))) {
            log_writers[writer_index].reset(new rotating_log_writer(
                logfile[writer_index], static_cast<std::size_t>(log_max_size_kb) * 1024U,
                log_archive_count));
            std::string open_error;
            if (!log_writers[writer_index]->open(open_error)) {
                log_writers[writer_index].reset();
                log_health.record_failure(now, text.size(), open_error);
                return;
            }
            const int writer_count = log_mode == log_mode_t::combined ? 1 : 2;
            for (int i = 0; i < writer_count; ++i) {
                if (log_writers[i]) continue;
                log_writers[i].reset(new rotating_log_writer(
                    logfile[i], static_cast<std::size_t>(log_max_size_kb) * 1024U,
                    log_archive_count));
                std::string sibling_error;
                if (!log_writers[i]->open(sibling_error)) {
                    log_writers[i].reset();
                    log_health.record_failure(now, 0, sibling_error);
                }
            }
            bool all_writers_open = true;
            for (int i = 0; i < writer_count; ++i)
                all_writers_open = all_writers_open && !!log_writers[i];
            if (log_health.degraded && all_writers_open) {
                PM_TINY_DLOG_INFO("program %s log recovered", name.c_str());
                log_health.record_recovery();
            }
        }
        if (!log_writers[writer_index]) {
            log_health.dropped_bytes += text.size();
            return;
        }
        std::string error;
        if (!log_writers[writer_index]->append(text.data(), text.size(), error)) {
            log_writers[writer_index].reset();
            const bool changed = !log_health.degraded || log_health.last_error != error;
            log_health.record_failure(now, text.size(), error);
            if (changed) PM_TINY_DLOG_ERROR("program %s log degraded: %s", name.c_str(), error.c_str());
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
                PM_TINY_DLOG_DEBUG("pid:%d pipe fd %d closed\n",
                                    this->instance.last_pid, fd);
                fd = -1;
                break;
            } else {
                std::string msg_content;
                int msg_type = 1;
                int remaining_bytes = std::min(nread, 64 * 1024);
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
                        } else {
                            msg_content += pure_text;
                        }
                        remaining_bytes -= rc;
                    } else if ((rc == -1 && errno != EINTR)) {
                        PM_TINY_DLOG_ERROR_ERRNO("name:%s pid:%d fdin:%d fdout:%d read",
                                                 this->name.c_str(), this->instance.pid, fd, -1);
                        break;
                    }
                } while (remaining_bytes > 0);

                if (!msg_content.empty()) {
                    this->write_msg_to_sessions(msg_type, msg_content);
                }
            }
        } while (killed);
    }

    void prog_info_t::write_cache_log_to_session(session_t *session) {
        const std::string prev_log = log_tail.snapshot();
        if (!prev_log.empty()) {
            auto frames = str_to_frames(1, prev_log);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                session->write_stream_frame(frames[i], 0, true);
            }
        }
    }

    void prog_info_t::write_msg_to_sessions(int msg_type, const std::string &msg_content) {
        log_tail.append(msg_content.data(), msg_content.size());
        if (!this->sessions.empty()) {
            auto frames = str_to_frames(msg_type, msg_content);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                for (auto &session: sessions) {
                    session->write_stream_frame(frames[i], 0, msg_type != 0 || i + 1 < frames.size());
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
            PM_TINY_DLOG_ERROR_ERRNO("kill");
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
            session->mark_close();
            session->set_prog(nullptr);
        }
        this->sessions.clear();
        fail_pending_log_sessions("log wait canceled before the next generation started");
    }

    void prog_info_t::wait_for_next_log_generation(const std::shared_ptr<session_t> &session) {
        pending_log_sessions.emplace_back(session);
    }

    void prog_info_t::activate_pending_log_sessions() {
        for (const auto &weak_session : pending_log_sessions) {
            auto session = weak_session.lock();
            if (!session || session->is_close()) continue;
            auto response_frame = std::make_unique<frame_t>();
            fappend_value<std::int32_t>(*response_frame, 0);
            fappend_value(*response_frame, std::string("success"));
            program_log_response response;
            response.mode = log_request_mode::live;
            response.generation = instance.generation;
            response.last_pid = instance.last_pid;
            append_program_log_response(*response_frame, response);
            add_session(session.get());
            session->write_frame(response_frame);
        }
        pending_log_sessions.clear();
    }

    void prog_info_t::fail_pending_log_sessions(const std::string &message) {
        for (const auto &weak_session : pending_log_sessions) {
            auto session = weak_session.lock();
            if (!session || session->is_close()) continue;
            auto response_frame = std::make_unique<frame_t>();
            fappend_value<std::int32_t>(*response_frame, -1);
            fappend_value(*response_frame, message);
            session->write_frame(response_frame);
        }
        pending_log_sessions.clear();
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
