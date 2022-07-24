//
// Created by luo on 2021/10/6.
//
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <memory>
#include <iostream>
#include <sys/time.h>
#include <pwd.h>
#include <string>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <limits.h>
#include <stdlib.h>
#include <getopt.h>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny_helper.h"
#include "pm_tiny.h"
#include "frame_stream.hpp"
#include "string_utils.h"
#include "memory_util.h"

using pm_tiny::operator<<;

struct proc_info_t {
    int pid;
    std::string name;
    std::string work_dir;
    std::string command;
    int restart_count;
    int state;
    long long vm_rss_kib;
};

std::ostream &operator<<(std::ostream &os, proc_info_t const &p) {
    os << p.pid << " " << p.name << " "
       << p.work_dir << " " << p.command << " "
       << p.restart_count << " " << p.state;
    return os;
}
void loop_read_show_process_log(pm_tiny::session_t &session);

std::string pm_state_to_str(int state) {
    switch (state) {
        case PM_TINY_PROG_STATE_NO_RUN:
            return "offline";
        case PM_TINY_PROG_STATE_RUNING:
            return "online";
        case PM_TINY_PROG_STATE_STARTUP_FAIL:
            return "startfail";
        case PM_TINY_PROG_STATE_REQUEST_STOP:
            return "stop";
        case PM_TINY_PROG_STATE_STOPED:
            return "stoped";
        case PM_TINY_PROG_STATE_EXIT:
            return "exit";
        default:
            return "Unkown";
    }
}

void show_msg(int code, const std::string &msg);

void display_proc_infos(pm_tiny::session_t &session) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x23);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);

    if (rf) {
        std::vector<proc_info_t> proc_infos;
        pm_tiny::iframe_stream ifs(*rf);
        int num;
        ifs >> num;
        proc_infos.resize(num);
        //n pid:name:workdir:command:restart_count:state:vm_rss_kib
        for (int i = 0; i < num; i++) {
            ifs >> proc_infos[i].pid;
            ifs >> proc_infos[i].name;
            ifs >> proc_infos[i].work_dir;
            ifs >> proc_infos[i].command;
            ifs >> proc_infos[i].restart_count;
            ifs >> proc_infos[i].state;
            ifs>>proc_infos[i].vm_rss_kib;
        }
        const int COLUMN_NUM = 7;
        int column_widths[COLUMN_NUM];
        std::string heads[COLUMN_NUM] = {"pid", "name", "cwd", "command", "restart", "state","memory"};
        printf("Total:%ld\n", proc_infos.size());
        for (int i = 0; i < COLUMN_NUM; i++) {
            column_widths[i] = (int) heads[i].length();
        }
        for (int i = 0; i < proc_infos.size(); i++) {
//            std::cout << i << " " << proc_infos[i] << std::endl;
            std::string s_pid = std::to_string(proc_infos[i].pid);
            if (s_pid.length() > column_widths[0]) {
                column_widths[0] = (int) s_pid.length();
            }
            if (proc_infos[i].name.length() > column_widths[1]) {
                column_widths[1] = (int) proc_infos[i].name.length();
            }
            if (proc_infos[i].work_dir.length() > column_widths[2]) {
                column_widths[2] = (int) proc_infos[i].work_dir.length();
            }
            if (proc_infos[i].command.length() > column_widths[3]) {
                column_widths[3] = (int) proc_infos[i].command.length();
            }
            std::string s_restart_count = std::to_string(proc_infos[i].restart_count);
            if (s_restart_count.length() > column_widths[4]) {
                column_widths[4] = (int) s_restart_count.length();
            }
            std::string s_state = pm_state_to_str(proc_infos[i].state);
            if (s_state.length() > column_widths[5]) {
                column_widths[5] = (int) s_state.length();
            }
            std::string mem_size = pm_tiny::utils::memory::to_human_readable_size(proc_infos[i].vm_rss_kib);
            if (mem_size.length() > column_widths[6]) {
                column_widths[6] = (int) mem_size.length();
            }
        }
        const int TTY_WIDTH = 132;
        int column_sum_width = std::accumulate(std::begin(column_widths), std::end(column_widths), 0);
        int avg_width = TTY_WIDTH / COLUMN_NUM;
        while (column_sum_width > TTY_WIDTH) {
            auto overval = TTY_WIDTH - column_sum_width;
            auto max_idx = std::distance(std::begin(column_widths),
                                         std::max_element(std::begin(column_widths), std::end(column_widths)));
            column_widths[max_idx] = std::max(column_widths[max_idx] - overval, avg_width);
            column_sum_width = std::accumulate(std::begin(column_widths), std::end(column_widths), 0);
        }
        auto print_split_line = [&column_widths](char sc) {
            for (int i = 0; i < COLUMN_NUM; i++) {
                if (i == 0) {
                    printf(" ");
                } else {
                    printf("%c", sc);
                }
                for (int k = 0; k < column_widths[i]; k++) {
                    printf("%c", sc);
                }
                printf("%c%c", sc, sc);
                if (i == COLUMN_NUM - 1) {
                    printf("\n");
                }
            }
        };
        auto print_table_head = [&column_widths, &heads]() {
            for (int i = 0; i < COLUMN_NUM; i++) {
                if (i == 0) {
                    printf("|");
                }
                printf(" \033[1;36m%-*s\033[0m |", column_widths[i], heads[i].c_str()
                );
                if (i == COLUMN_NUM - 1) {
                    printf("\n");
                }
            }
        };
        print_split_line('-');
        print_table_head();
        print_split_line('-');
        auto truncate_long_str = [](const std::string &str, int maxlen) -> std::string {
            if (str.length() > maxlen && maxlen > 3) {
                return "..." + str.substr(str.length() - maxlen + 3);
            } else {
                return str;
            }
        };

        for (int i = 0; i < proc_infos.size(); i++) {
            auto &p = proc_infos[i];
            std::string s_state = pm_state_to_str(p.state);
            s_state = truncate_long_str(s_state, column_widths[5]);
            if (p.state == PM_TINY_PROG_STATE_RUNING) {
                s_state = "\033[1;32m" + s_state + "\033[0m";
            } else {
                s_state = "\033[1;31m" + s_state + "\033[0m";
            }
            std::string mem_size_str = pm_tiny::utils::memory::to_human_readable_size(proc_infos[i].vm_rss_kib);
            printf("| %-*d | %-*s | %-*s | %-*s | %-*d | %-*s | %-*s |\n", column_widths[0], p.pid,
                   column_widths[1], truncate_long_str(p.name, column_widths[1]).c_str(),
                   column_widths[2], truncate_long_str(p.work_dir, column_widths[2]).c_str(),
                   column_widths[3], truncate_long_str(p.command, column_widths[3]).c_str(),
                   column_widths[4], p.restart_count,
                   column_widths[5] + 11, s_state.c_str(),column_widths[6],mem_size_str.c_str()
            );
        }
        if (!proc_infos.empty()) {
            print_split_line('-');
        }

    } else {
        printf("no data read\n");
    }
}

void stop_proc(pm_tiny::session_t &session, const std::string &app_name) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x24);
    pm_tiny::fappend_value(*f, app_name);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);
    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        show_msg(code, msg);
        display_proc_infos(session);
    }
}

void start_proc(pm_tiny::session_t &session,
                const std::string &cmd,
                const std::string &named,int kill_timeout,
                const std::string&run_as,bool show_log) {
    std::vector<std::string> args;
    mgr::utils::split(cmd, {' ', '\t'}, std::back_inserter(args));
    std::for_each(args.begin(), args.end(), mgr::utils::trim);
    args.erase(std::remove_if(args.begin(), args.end(),
                              [](const std::string &x) { return x.empty(); }), args.end());
    if (args.empty()) {
        fprintf(stderr, "app name is required\n");
        return;
    }
    auto exe_path = args[0];
    char app_realpath[PATH_MAX];
    int local_resolved = 0;
    if (realpath(exe_path.c_str(), app_realpath) != nullptr) {
        exe_path = app_realpath;
        args[0] = exe_path;
        local_resolved = 1;
    }
    std::string command = std::accumulate(args.begin(), args.end(), std::string(""),
                                          [](const std::string &s1, const std::string &s2) {
                                              return s1 + (s2 + " ");
                                          });
    mgr::utils::trim(command);
    std::string filename = exe_path;
    std::string ext_name = "";
    auto slash_idx = exe_path.rfind('/');
    if (slash_idx != std::string::npos) {
        filename = exe_path.substr(slash_idx + 1);
    }
    std::string name = filename;
    auto dot_idx = filename.rfind('.');
    if (dot_idx != std::string::npos) {
        name = filename.substr(0, dot_idx);
        ext_name = filename.substr(dot_idx + 1);
    }
//name:cwd:command local_resolved envp
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x25);
    if (named.empty()) {
        pm_tiny::fappend_value(*f, name);
    } else {
        pm_tiny::fappend_value(*f, named);
    }
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    if (command.find(cwd) == 0) {
        command = "." + command.substr(strlen(cwd));
    }
    pm_tiny::fappend_value(*f, cwd);
    pm_tiny::fappend_value(*f, command);
    pm_tiny::fappend_value(*f, local_resolved);
    int env_num = 0;
    for (char **env = ::environ; *env != nullptr; env++) {
        env_num++;
    }
    pm_tiny::fappend_value(*f, env_num);
    for (char **env = ::environ; *env != nullptr; env++) {
        char *thisEnv = *env;
        pm_tiny::fappend_value(*f, thisEnv);
    }
    pm_tiny::fappend_value(*f,kill_timeout);
    pm_tiny::fappend_value(*f,run_as);
    pm_tiny::fappend_value<int>(*f, show_log ? 1 : 0);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);
    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        if (code != 1) {
            show_msg(code, msg);
            display_proc_infos(session);
        } else {
            loop_read_show_process_log(session);
        }
    }
}


void save_proc(pm_tiny::session_t &session) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x26);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);

    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        show_msg(code, msg);
    }
}

void show_msg(int code, const std::string &msg) {
    if (code != 0) {
        fprintf(stderr, "\033[31mFail(%d):%s\n\033[0m", code, msg.c_str());
    } else {
        printf("\033[32mSuccess\n\033[0m");
    }
}


void delete_prog(pm_tiny::session_t &session, const std::string &app_name) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x27);
    pm_tiny::fappend_value(*f, app_name);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);
    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        show_msg(code, msg);
        display_proc_infos(session);
    }
}


void restart_prog(pm_tiny::session_t &session, const std::string &app_name) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x28);
    pm_tiny::fappend_value(*f, app_name);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);
    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        show_msg(code, msg);
        display_proc_infos(session);
    }
}

void show_version(pm_tiny::session_t&session){
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(0x29);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);

    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        std::string version;
        ifs >> version;
        fprintf(stdout, "%s\n", version.c_str());
    }
}
void loop_read_show_process_log(pm_tiny::session_t &session){
    int msg_type = 0;
    std::string msg_content;
    do {
        auto rf = session.read_frame(1);
        pm_tiny::iframe_stream ifs(*rf);
        ifs >> msg_type;
        ifs >> msg_content;
        printf("%s", msg_content.c_str());
        fflush(stdout);
    } while (msg_type != 0);
}

void show_prog_log(pm_tiny::session_t &session, const std::string &app_name) {
    pm_tiny::frame_ptr_t f = std::make_shared<pm_tiny::frame_t>();
    f->push_back(PM_TINY_FRAME_TYPE_SHOW_LOG);
    pm_tiny::fappend_value(*f, app_name);
    session.write_frame(f, 1);
    auto rf = session.read_frame(1);
    if (rf) {
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code;
        ifs >> msg;
        if (code != 0) {
            show_msg(code, msg);
        } else {
            loop_read_show_process_log(session);
        }
    }
}

void show_usage(int argc, char *argv[]) {
    fprintf(stdout, "usage: %s <command> [options]\n\n", argv[0]);
    fprintf(stdout, "- Start and add a process to the pm_tiny process list:\n\n");
    fprintf(stdout, "\033[36m$ pm start \"node test.js arg0 arg1\" --name app_name [--kill_timeout second] [--log]\n\n\033[0m");
    fprintf(stdout, "- Show the process list:\n\n");
    fprintf(stdout, "\033[36m$ pm ls\n\n\033[0m");
    fprintf(stdout, "- Show the process output:\n\n");
    fprintf(stdout, "\033[36m$ pm log app_name\n\n\033[0m");
    fprintf(stdout, "- Stop and delete a process from the pm process list:\n\n");
    fprintf(stdout, "\033[36m$ pm delete app_name\n\n\033[0m");
    fprintf(stdout, "- Stop, start and restart a process from the process list:\n\n");
    fprintf(stdout, "\033[36m$ pm stop app_name\n$ pm start app_name\n$ pm restart app_name\n\n\033[0m");
    fprintf(stdout, "- Save the process configuration:\n\n");
    fprintf(stdout, "\033[36m$ pm save\n\n\033[0m");
}

struct cmd_opt_t {
    char cmd[50];
    int argc_required = 0;
    char **argv = nullptr;
    int argc = 0;
    std::shared_ptr<pm_tiny::session_t> session;

    void *(*fun)(cmd_opt_t &cmd_opt);

    void *operator()() {
        return this->fun(*this);
    }
};

void *test_fun(cmd_opt_t &cmd_opt) {
    printf("method not implemented\n");
    return nullptr;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        show_usage(argc, argv);
        exit(EXIT_FAILURE);
    }

    auto make_cmd_opt = [](const char *cmd,
                           void *(*fun)(cmd_opt_t &cmd_opt) = test_fun,
                           int argc_required = 1) {
        cmd_opt_t opt{};
        strncpy(opt.cmd, cmd, sizeof(opt.cmd));
        opt.fun = fun;
        opt.argc_required = argc_required;
        return opt;
    };
    void *(*list_fun)(cmd_opt_t &) = [](cmd_opt_t &cmd_opt) -> void * {
        display_proc_infos(*cmd_opt.session);
        return nullptr;
    };
    auto stop_fun = [](cmd_opt_t &cmd_opt) -> void * {
        stop_proc(*cmd_opt.session, cmd_opt.argv[1]);
        return nullptr;
    };
    auto start_fun = [](cmd_opt_t &cmd_opt) -> void * {
        std::string named;
        std::string kill_timeout_str = "3";
        std::string run_as;
        int kill_timeout=3;
        int option_index = 0;
        bool show_log=false;
        optind = 2;
        static struct option long_options[] = {
                {"name", required_argument, 0, 0},
                {"kill_timeout", required_argument, 0, 0},
                {"run_as", required_argument, 0, 0},
                {"log", no_argument, 0, 0},
                {nullptr, 0,                0, 0}
        };
        int c;
        while (true) {
            c = getopt_long(cmd_opt.argc, cmd_opt.argv, "",
                            long_options, &option_index);
            if (c == -1)
                break;
            switch (c) {
                case 0:
//                    printf("option %s", long_options[option_index].name);
//                    if (optarg)
//                        printf(" with arg %s", optarg);
//                    printf("\n");
                    if (option_index == 0) {
                        named = optarg;
                    } else if (option_index == 1) {
                        kill_timeout_str = optarg;
                    } else if (option_index == 2) {
                        run_as = optarg;
                    } else {
                        show_log = true;
                    }
                    break;

                case '?':
                default:
                    exit(EXIT_FAILURE);
            }
        }
        try {
            kill_timeout = std::stoi(kill_timeout_str);
        } catch (const std::invalid_argument &e) {
            kill_timeout = 3;
        }
        start_proc(*cmd_opt.session, cmd_opt.argv[1], named,kill_timeout,run_as,show_log);
        return nullptr;
    };
    auto version_fun = [](cmd_opt_t &cmd_opt) -> void * {
        show_version(*cmd_opt.session);
        return nullptr;
    };

    auto handle_cmdline = [&make_cmd_opt,
            &version_fun](int argc, char *argv[])
            -> cmd_opt_t {
        int c;
        int digit_optind = 0;
        bool found = false;
        while (true) {
            int this_option_optind = optind ? optind : 1;
            int option_index = 0;
            static struct option long_options[] = {
                    {"version", no_argument, 0, 0},
                    {0, 0,                   0, 0}
            };

            c = getopt_long(argc, argv, "vV",
                            long_options, &option_index);
            if (c == -1)
                break;

            switch (c) {
                case 0:
//                  printf("option %s", long_options[option_index].name);
                    found = true;
                    break;
                case 'v':
                case 'V':
                    found = true;
                    break;
                case '?':
                    break;

                default:
                    printf("?? getopt returned character code 0%o ??\n", c);
            }
        }
        if (found) {
            return make_cmd_opt("show_version", version_fun, 0);
        } else {
            show_usage(argc, argv);
            exit(EXIT_FAILURE);
        }
    };
    auto save_fun = [](cmd_opt_t &cmd_opt) -> void * {
        save_proc(*cmd_opt.session);
        return nullptr;
    };
    auto delete_fun = [](cmd_opt_t &cmd_opt) -> void * {
        delete_prog(*cmd_opt.session, cmd_opt.argv[1]);
        return nullptr;
    };
    auto restart_fu = [](cmd_opt_t &cmd_opt) -> void * {
        restart_prog(*cmd_opt.session, cmd_opt.argv[1]);
        return nullptr;
    };


    auto log_fun = [](cmd_opt_t &cmd_opt) -> void * {
        show_prog_log(*cmd_opt.session, cmd_opt.argv[1]);
        return nullptr;
    };


    std::vector<cmd_opt_t> cmd_opts{
            make_cmd_opt("start", start_fun, 1),
            make_cmd_opt("stop", stop_fun, 1),
            make_cmd_opt("restart", restart_fu, 1),
            make_cmd_opt("delete", delete_fun, 1),
            make_cmd_opt("list", list_fun, 0),
            make_cmd_opt("ls", list_fun, 0),
            make_cmd_opt("status", list_fun, 0),
            make_cmd_opt("save", save_fun, 0),
            make_cmd_opt("log", log_fun, 1),

    };
    cmd_opt_t *cmd_opt = nullptr;
    int found_command = false;
    for (cmd_opt_t &opt: cmd_opts) {
        if (strcmp(opt.cmd, argv[1]) == 0) {
            found_command = true;
            if ((argc - 2) >= opt.argc_required) {
                opt.argc = argc - 1;
                opt.argv = argv + 1;
                cmd_opt = &opt;
            }
            break;
        }
    }
    if (!found_command && argv[1][0] == '-') {
        static auto opt = handle_cmdline(argc, argv);
        cmd_opt = &opt;
        found_command = true;
    }

    if (!found_command) {
        fprintf(stderr, "\033[31mCommand not found:%s\n\n\033[0m", argv[1]);
        show_usage(argc, argv);
        exit(EXIT_FAILURE);
    }
    if (cmd_opt == nullptr) {
        show_usage(argc, argv);
        exit(EXIT_FAILURE);
    }


    if (pm_tiny::set_sigaction(SIGPIPE, SIG_IGN) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_un serun;
    int len;
    int sockfd;

    std::string pm_tiny_home_dir = pm_tiny::get_pm_tiny_home_dir("");
    auto sock_path = pm_tiny_home_dir + "/pm_tinyd.sock";

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("client socket error");
        exit(1);
    }
    memset(&serun, 0, sizeof(serun));
    serun.sun_family = AF_UNIX;
    strcpy(serun.sun_path, sock_path.c_str());
    len = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
    if (connect(sockfd, (struct sockaddr *) &serun, len) < 0) {
        perror("connect error");
        exit(1);
    }
    auto session = std::make_shared<pm_tiny::session_t>(sockfd, 0);
    cmd_opt->session = session;
    (*cmd_opt)();
    close(sockfd);
    return 0;
}