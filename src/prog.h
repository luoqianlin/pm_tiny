//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

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
#include "logger.hpp"
#include <memory>
#include "../src/string_utils.h"
#include <math.h>
#include <climits>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny.h"
#include "pm_tiny_helper.h"
#include "frame_stream.hpp"
#include "procinfo.h"
#include "graph.h"
#include "pm_tiny_enum.h"
#include <unordered_map>
#include <unordered_set>
namespace pm_tiny {
    class session_t;
    class pm_tiny_server_t;
    struct prog_info_t;

//    using prog_ptr_t = std::shared_ptr<prog_info_t>;
    using prog_ptr_t = prog_info_t *;
    using proglist_t = std::list<prog_ptr_t>;
    using task_fun_t = std::function<void(pm_tiny_server_t &)>;

    struct prog_info_t {
        pid_t pid = -1;
        pid_t backup_pid = -1;
        int rpipefd[2]{-1, -1};
        std::vector<std::string> args;
        int64_t last_startup_ms = 0;
        int64_t last_dead_time_ms = 0;
        int last_wstatus = 0;
        int dead_count = 0;
        int dead_count_timer = 0;
        std::string name;
        std::string logfile[2];
        std::string work_dir;
        int logfile_fd[2]{-1, -1};
        int64_t logfile_size[2]{0, 0};//bytes
        int64_t logfile_maxsize = 4 * 1024 * 1024L;
        int logfile_count = 3;
        int64_t moniter_duration_threshold = 60 * 1000L;
        int64_t min_lifetime_threshold = 100L;
        int moniter_duration_max_dead_count = -1;
        int state = PM_TINY_PROG_STATE_NO_RUN;
        std::vector<std::string> envs;
        int kill_timeout_sec = 3;//3s
        std::string run_as;
        const int MAX_CACHE_LOG_LEN = 4096;//4kb
        std::vector<char> cache_log;
        std::string residual_log;
        int64_t request_stop_timepoint = 0;

        std::vector<std::string> depends_on;
        int start_timeout = 0;//unit second,0 immediately available, -1 waiting for external notification
        failure_action_t failure_action = failure_action_t::SKIP;
        bool daemon = true;
        int heartbeat_timeout = -1;//The unit is second, its value <=0 means disable
        int64_t last_tick_timepoint = 0;//milliseconds

        std::vector<session_t *> sessions;

        std::vector<task_fun_t> kill_pendingtasks;

        void close_pipefds();

        void close_logfds();

        void set_state(int s);
        int64_t update_count_timer();

        bool is_reach_max_num_death();
        /**
         * 监管的程序运行结束后会关闭pipefd,
         * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
         * */
        void close_fds();

        void write_prog_exit_message();

        std::string get_desc_name() const;

        void init_prog_log();

        void read_pipe(int i, int killed = 0);

        std::string remove_ANSI_escape_code(const std::string &text);

        void redirect_output_log(int i, std::string text);

        bool remove_session(session_t *session);

        void write_msg_to_sessions(int msg_type, const std::string &msg_content);

        void write_cache_log_to_session(session_t *session);

        bool is_sessions_writeable();

        void add_session(session_t *session);

        static std::string log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus);

        bool is_kill_timeout() const;

        bool is_start_timeout() const;
        bool is_tick_timeout() const;

        void async_force_kill();

        void async_kill_prog();

        void execute_penddingtasks(pm_tiny_server_t &pm_tiny_server);

        void detach_sessions();

        bool  is_cfg_equal(const prog_ptr_t prog)const;
    };

    inline void delete_prog(prog_ptr_t prog) {
        delete prog;
    }

    inline void delete_proglist(proglist_t &pl) {
        for (auto p: pl) {
            delete p;
        }
    }
    struct prog_info_wrapper_t{
        prog_info_t * prog_info;
    };
    using prog_graph_t = Graph<prog_info_wrapper_t>;

    std::ostream &operator<<(std::ostream &os, const prog_info_wrapper_t &prog_info);
    class ProgDAG{
    public:
        std::unique_ptr<prog_graph_t> graph;
    public:
        ProgDAG(std::unique_ptr<prog_graph_t> graph);
        bool is_traversal_complete() const;
        void show_in_degree_info()const;
        void show_depends_info()const;
        proglist_t start();
        proglist_t next(const proglist_t& pl);
        void remove(const proglist_t& pl);
    };
    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog);
    int get_min_start_timeout(const proglist_t &start_progs);
    std::unique_ptr<ProgDAG>
            check_prog_info(const std::vector<prog_ptr_t> &prog_cfgs);
    void async_kill_prog(pm_tiny_server_t &pm_tiny_server,prog_ptr_t&prog);
}
#endif //PM_TINY_PROG_H
