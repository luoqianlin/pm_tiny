//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#ifndef PM_TINY_PM_TINY_SERVER_H
#define PM_TINY_PM_TINY_SERVER_H

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
#include "string_utils.h"
#include <math.h>
#include <climits>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny.h"
#include "pm_tiny_helper.h"
#include "frame_stream.hpp"
#include "prog.h"
#include "pm_tiny_utility.h"
#include "persistence_worker.h"
#include "daemon_config.h"

namespace pm_tiny {
    constexpr const char *pm_tiny_version = PM_TINY_VERSION;

    struct reload_config_t {
        proglist_t pl_;
        dependency_graph graph_;
        bool valid_ = false;
        std::string error_message_;

        reload_config_t(proglist_t pl, dependency_graph graph, bool valid, std::string error_message)
                : pl_(std::move(pl)), graph_(std::move(graph)), valid_(valid),
                  error_message_(std::move(error_message)) {}

        bool is_valid() const {
            return valid_;
        }
    };
    class pm_tiny_server_t {
    public:
        std::string pm_tiny_home_dir;
        std::string pm_tiny_log_file;
        std::string pm_tiny_prog_cfg_file;
        std::string pm_tiny_app_log_dir;
        std::string pm_tiny_app_environ_dir;
        std::string pm_tiny_sock_file;
        std::string pm_tiny_lock_file;
        CloseableFd lmkdFd;
        bool uds_abstract_namespace;
        std::vector<unsigned int> allowed_uids;
        std::vector<unsigned int> allowed_gids;
        std::shared_ptr<process_tree_controller> process_tree;
        proglist_t pm_tiny_progs;
        dependency_graph dependency_graph_;
        dependency_runtime dependency_runtime_;
        int server_exit = 0;

        std::vector<pm_tiny::session_ptr_t> sessions;
        std::unique_ptr<reload_config_t> reload_config;

        std::vector<std::weak_ptr<pm_tiny::session_t>> wait_reload_sessions;
        std::vector<std::weak_ptr<pm_tiny::session_t>> wait_save_sessions;
        persistence_worker persistence;
        daemon_config effective_daemon_config;
        daemon_cli_options daemon_options;
        std::int64_t started_monotonic_ms = 0;
        bool subreaper_enabled = false;
        std::string subreaper_error;

        int parse_cfg();

        std::unique_ptr<reload_config_t>
        parse_cfg2();
        bool is_prog_depends_valid(prog_ptr_t prog);

        void parse_app_environ(const std::string &name,
                               std::vector<std::string> &envs) const;

        int parse_cfg(proglist_t &progs) const;

        std::unique_ptr<prog_info_t> create_prog(const prog_cfg_t &config,
                                                 const std::vector<std::string> &envs) const;

        int start_and_add_prog(const prog_ptr_t &prog);

        int start_prog(const prog_ptr_t &prog);
        int request_start(const prog_ptr_t &prog);
        void mark_dependency_stopped(const prog_ptr_t &prog);

        int save_proc_to_cfg();
        bool begin_save_proc_to_cfg();
        bool poll_save_proc_to_cfg(int &result);
        bool persistence_busy() const;
        void wait_for_persistence();

        void restart_startfailed();

        void remove_prog(prog_ptr_t&prog);
        std::vector<std::string> dependency_dependents(const std::string &name) const;

        void async_kill_prog(prog_ptr_t&prog_);
        void trigger_DAG_traversal_next_node(const prog_ptr_t&prog);
        void spawn1(proglist_t& started_progs);

        void spawn();

        void close_fds();

        int real_spawn_prog(pm_tiny::prog_info_t &prog);

        int spawn_prog(pm_tiny::prog_info_t &prog);

        prog_ptr_t find_prog(int pid);

        void flag_startup_fail(prog_ptr_t&prog);

        void show_prog_depends_info() const;

        void request_quit();

        void swap_reload_config();

        bool is_reloading() const;

        bool is_exiting() const;

        void kill_all_prog();

        bool init_process_tree(const std::string &mode, const std::string &root);

    private:
        proglist_t spawn0(proglist_t& start_progs);
        prog_ptr_t find_prog(const std::string &name) const;
        proglist_t progs_from_names(const std::vector<std::string> &names) const;
        bool rebuild_dependency_graph(const proglist_t &progs, std::string &error_message);


    };
}
#endif //PM_TINY_PM_TINY_SERVER_H
