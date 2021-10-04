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
#include "prog.h"

namespace pm_tiny {
    constexpr const char *pm_tiny_version = PM_TINY_VERSION;

    struct reload_config_t {
        proglist_t pl_;
        std::unique_ptr<ProgDAG> dag_;

        reload_config_t(proglist_t pl, std::unique_ptr<ProgDAG> dag)
                : pl_(std::move(pl)), dag_(std::move(dag)) {}

        bool is_valid() const {
            return dag_ != nullptr;
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
        bool uds_abstract_namespace;
        proglist_t pm_tiny_progs;
        std::unique_ptr<ProgDAG> progDAG;
        int server_exit = 0;

        std::vector<pm_tiny::session_ptr_t> sessions;
        std::unique_ptr<reload_config_t> reload_config;

        std::vector<std::weak_ptr<pm_tiny::session_t>> wait_reload_sessions;

        int parse_cfg();

        std::unique_ptr<reload_config_t>
        parse_cfg2();
        bool is_prog_depends_valid(prog_ptr_t prog);

        void parse_app_environ(const std::string &name,
                               std::vector<std::string> &envs) const;

        int parse_cfg(proglist_t &progs) const;

        std::unique_ptr<prog_info_t> create_prog(const std::string &app_name,
                               const std::string &cwd,
                               const std::string &command,
                               const std::vector<std::string> &envs,
                               int kill_timeout_sec,
                               const std::string&run_as) const;

        int start_and_add_prog(const prog_ptr_t &prog);

        int start_prog(const prog_ptr_t &prog);

        int save_proc_to_cfg();

        void restart_startfailed();

        void remove_prog(prog_ptr_t&prog);

        void async_kill_prog(prog_ptr_t&prog_);
        void trigger_DAG_traversal_next_node(const prog_ptr_t&prog);
        void spawn1(proglist_t& started_progs);

        void spawn();

        void close_fds();

        int real_spawn_prog(pm_tiny::prog_info_t &prog);

        int spawn_prog(pm_tiny::prog_info_t &prog);

        prog_ptr_t find_prog(int pid);

        void remove_from_DAG(const prog_ptr_t& prog);
        void flag_startup_fail(prog_ptr_t&prog) const;

        void show_prog_depends_info() const;

        void request_quit();

        void swap_reload_config();

        bool is_reloading() const;

        bool is_exiting() const;

    private:
        proglist_t spawn0(proglist_t& start_progs);


    };
}
#endif //PM_TINY_PM_TINY_SERVER_H
