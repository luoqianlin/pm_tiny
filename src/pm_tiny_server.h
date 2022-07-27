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

    using prog_ptr_t = std::shared_ptr<pm_tiny::prog_info_t>;
    using proglist_t = std::list<prog_ptr_t>;

    class pm_tiny_server_t {
    public:
        std::string pm_tiny_home_dir;
        std::string pm_tiny_log_file;
        std::string pm_tiny_cfg_file;
        std::string pm_tiny_app_log_dir;
        std::string pm_tiny_app_environ_dir;
        proglist_t pm_tiny_progs;
        int server_exit = 0;

        std::vector<pm_tiny::session_ptr_t> sessions;



        int parse_cfg();

        void parse_app_environ(const std::string &name,
                               std::vector<std::string> &envs) const;

        int parse_cfg(proglist_t &progs) const;

        prog_ptr_t create_prog(const std::string &app_name,
                               const std::string &cwd,
                               const std::string &command,
                               const std::vector<std::string> &envs,
                               int kill_timeout_sec,
                               const std::string&run_as) const;

        int start_and_add_prog(const prog_ptr_t &prog);

        int start_prog(const prog_ptr_t &prog);

        int save_proc_to_cfg();

        void restart_startfailed();

        void spawn();

        void close_fds();

        int real_spawn_prog(pm_tiny::prog_info_t &prog);

        int spawn_prog(pm_tiny::prog_info_t &prog);

        prog_ptr_t find_prog(int pid);

    };
}
#endif //PM_TINY_PM_TINY_SERVER_H
