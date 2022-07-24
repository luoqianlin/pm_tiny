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

namespace pm_tiny {
    class session_t;
    struct prog_info_t {
        pid_t pid = -1;
        pid_t backup_pid = -1;
        int rpipefd[2]{-1, -1};
        std::vector <std::string> args;
        int64_t last_startup_ms = 0;
        int64_t last_dead_time_ms = 0;
        int last_wstatus = 0;
        int pendding_signal = 0;
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
        std::vector <std::string> envs;
        int kill_timeout_sec = 3;//3s
        std::string run_as;
        const int MAX_CACHE_LOG_LEN = 4096;//4kb
        std::vector<char> cache_log;

        std::vector<session_t*> sessions;

        void close_pipefds();

        void close_logfds();

        void set_state(int s);

        /**
         * 监管的程序运行结束后会关闭pipefd,
         * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
         * */
        void close_fds() ;

        std::string get_dsc_name() const ;

        void init_prog_log();

        void read_pipe(int i);

        bool remove_session(session_t *session);

        void write_msg_to_sessions(int msg_type,std::string&msg_content);

        bool is_sessions_writeable();

        void add_session(session_t *session);

       static std::string log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus);
    };
    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog);
}
#endif //PM_TINY_PROG_H
