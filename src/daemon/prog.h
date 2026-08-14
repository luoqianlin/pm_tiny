
#ifndef PM_TINY_PROG_H
#define PM_TINY_PROG_H

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>
#include <numeric>
#include <list>
#include <tuple>
#include <string.h>
#include <stdarg.h>
#include "daemon_log.h"
#include <memory>
#include <cstdint>
#include "string_utils.h"
#include <math.h>
#include <climits>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny.h"
#include "pm_tiny_helper.h"
#include "frame_stream.hpp"
#include "procinfo.h"
#include "dependency_graph.h"
#include "pm_tiny_enum.h"
#include "restart_policy.h"
#include <unordered_map>
#include <unordered_set>
#include "pm_tiny_utility.h"
#include "platform/linux/process_tree_controller.h"
#include "core/termination_job.h"
#include "program_log.h"
#include "rotating_log_writer.h"
namespace pm_tiny {
    struct managed_process_instance {
        uint64_t generation = 0;
        pid_t pid = -1;
        pid_t last_pid = -1;
        process_tree_handle tree;
        termination_phase termination = termination_phase::none;
        termination_job job;

        void begin(pid_t new_pid) {
            ++generation;
            pid = last_pid = new_pid;
            tree = process_tree_handle{};
            termination = termination_phase::none;
            job = termination_job{};
        }

        void reset_failed_start() {
            pid = last_pid = -1;
            tree = process_tree_handle{};
            termination = termination_phase::none;
            job = termination_job{};
        }
    };
    class session_t;
    class pm_tiny_server_t;
    struct prog_info_t;

//    using prog_ptr_t = std::shared_ptr<prog_info_t>;
    using prog_ptr_t = prog_info_t *;
    using proglist_t = std::list<prog_ptr_t>;
    using task_fun_t = std::function<void(pm_tiny_server_t &)>;
    struct generation_task {
        uint64_t generation;
        task_fun_t task;

        bool matches(uint64_t current_generation) const {
            return generation == current_generation;
        }
    };

    struct prog_info_t {
        // Asio pipe callbacks keep only a weak copy so queued callbacks can
        // detect that this raw-pointer-owned process object was destroyed.
        std::shared_ptr<const char> lifetime_token = std::make_shared<const char>(0);
        managed_process_instance instance;
        std::shared_ptr<process_tree_controller> tree_controller;
        int rpipefd[2]{-1, -1};
        std::string executable;
        std::vector<std::string> args;
        int64_t last_startup_ms = 0;
        int64_t last_dead_time_ms = 0;
        int last_wstatus = 0;
        bool has_last_exit = false;
        int dead_count = 0;
        int dead_count_timer = 0;
        std::string name;
        std::string logfile[2];
        std::string work_dir;
        std::unique_ptr<rotating_log_writer> log_writers[2];
        log_mode_t log_mode = log_mode_t::split;
        std::string log_dir;
        std::string log_file_name;
        int log_max_size_kb = 4096;
        int log_archive_count = 3;
        bounded_log_tail log_tail;
        log_sink_health log_health;
        int64_t moniter_duration_threshold = 60 * 1000L;
        int64_t min_lifetime_threshold = 100L;
        int moniter_duration_max_dead_count = -1;
        restart_policy_config restart_config;
        restart_policy_state restart_state;
        bool restart_pending = false;
        int64_t restart_due_ms = 0;
        int state = PM_TINY_PROG_STATE_NO_RUN;
        std::vector<std::string> envs;
        int kill_timeout_sec = 3;//3s
        std::string run_as;
        std::vector<std::string> env_vars;
        int oom_score_adj;

        std::string residual_log;

        std::vector<std::string> depends_on;
        int start_timeout = 0;//unit second,0 immediately available, -1 waiting for external notification
        failure_action_t failure_action = failure_action_t::SKIP;
        bool daemon = true;
        int heartbeat_timeout = -1;//The unit is second, its value <=0 means disable
        int64_t last_tick_timepoint = 0;//milliseconds
        bool use_pty = false;
        std::string process_tree_backend;
        bool process_tree_degraded = false;
        std::string process_tree_degradation_reason;
        std::string config_source = "file";

        std::vector<session_t *> sessions;

        std::vector<generation_task> kill_pendingtasks;

        void close_pipefds();

        void close_logfds();

        void set_state(int s);
        int64_t update_count_timer();

        bool is_reach_max_num_death();

        restart_decision plan_automatic_restart(int64_t now_ms, int64_t runtime_ms);
        void reset_restart_policy();
        /**
         * 监管的程序运行结束后会关闭pipefd,
         * 事件循环会监听到 pipefd 关闭，进而关闭 pipefd 和对应的日志文件 fd。
         * */
        void close_fds(const CloseableFd& lmkd);

        void write_prog_exit_message();

        std::string get_desc_name() const;

        void init_prog_log();

        void read_pipe(int i, int killed = 0);

        std::string remove_ANSI_escape_code(const std::string &text);

        void redirect_output_log(int i, std::string text);

        bool remove_session(session_t *session);

        void write_msg_to_sessions(int msg_type, const std::string &msg_content);

        void write_cache_log_to_session(session_t *session);

        void add_session(session_t *session);

        static std::string log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus);

        bool is_start_timeout() const;
        bool is_tick_timeout() const;

        void async_force_kill();

        termination_action poll_termination();
        termination_action mark_tree_draining();

        void signal_tree(int signo);

        void async_kill_prog();

        bool is_tree_empty() const;

        void execute_penddingtasks(pm_tiny_server_t &pm_tiny_server);

        void enqueue_after_termination(task_fun_t task);

        void detach_sessions();

    };

    inline void delete_prog(prog_ptr_t prog) {
        delete prog;
    }

    inline void delete_proglist(proglist_t &pl) {
        for (auto p: pl) {
            delete p;
        }
    }
    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog);
    bool build_prog_dependency_graph(const std::vector<prog_ptr_t> &progs,
                                     dependency_graph &graph,
                                     std::string &error_message);
    void async_kill_prog(pm_tiny_server_t &pm_tiny_server,prog_ptr_t&prog);
}
#endif //PM_TINY_PROG_H
