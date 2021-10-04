//
// Created by luo on 2021/10/6.
//
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <memory>
#include <iostream>
#include <string>
#include <numeric>
#include <getopt.h>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny_helper.h"
#include "frame_stream.hpp"
#include "pm_funcs.h"
#include "string_utils.h"
#include "pm_tiny_enum.h"
#include "ANSI_color.h"

sig_atomic_t pm_is_stop = 0;

void show_usage(int, char *argv[]) {
    fprintf(stdout, "usage: %s <command> [options]\n\n", argv[0]);
    fprintf(stdout, "- Start and add a process to the pm_tiny process list:\n\n");
    fprintf(stdout,
            PM_TINY_ANSI_COLOR_CYAN "$ %s start \"node test.js arg0 arg1\""
            " --name app_name [--log]" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout, "other options:\n"
                    "\t--kill_timeout <seconds>\n"
                    "\t--user <user>\n"
                    "\t--env_var <key=value>\n"
                    "\t--depends_on <other_apps>\n"
                    "\t--start_timeout <seconds>\n"
                    "\t--failure_action <skip|restart|reboot>\n"
                    "\t--heartbeat_timeout <seconds>\n"
                    "\t--no_daemon\n"
                    "\t--log\n\n");
    fprintf(stdout, "- Show the process list:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s ls" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout, "- Show the process output:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s log app_name" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout, "- Stop and delete a process from the pm process list:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s delete app_name" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout, "- Stop, start and restart a process from the process list:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s stop app_name\n$ %s start app_name [--log]\n"
                    "$ %s restart app_name [--log]" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0],argv[0],argv[0]);
    fprintf(stdout, "- Show the process configuration:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s inspect app_name" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout, "- Save the process configuration:\n\n");
    fprintf(stdout, PM_TINY_ANSI_COLOR_CYAN "$ %s save" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout,"- Reload the configuration file:\n\n");
    fprintf(stdout,PM_TINY_ANSI_COLOR_CYAN "$ %s reload" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
    fprintf(stdout,"- Shut down pm_tiny service:\n\n");
    fprintf(stdout,PM_TINY_ANSI_COLOR_CYAN "$ %s quit" PM_TINY_ANSI_COLOR_REST "\n\n",argv[0]);
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

void *test_fun(cmd_opt_t &) {
    printf("method not implemented\n");
    return nullptr;
}

int str2int(const std::string &str, int default_val) {
    if (str.empty()) {
        return default_val;
    }
    try {
        return std::stoi(str);
    } catch (const std::invalid_argument &e) {
        return default_val;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        show_usage(argc, argv);
        exit(EXIT_FAILURE);
    }
    using namespace pm_funcs;
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
    auto inspect_fun = [](cmd_opt_t &cmd_opt) -> void * {
        inspect_proc(*cmd_opt.session, cmd_opt.argv[1]);
        return nullptr;
    };
    auto reload_fun = [](cmd_opt_t &cmd_opt) -> void * {
        pm_tiny_reload(*cmd_opt.session, 1);
        return nullptr;
    };
    auto quit_fun = [](cmd_opt_t &cmd_opt) -> void * {
        pm_tiny_quit(*cmd_opt.session);
        return nullptr;
    };
    auto start_fun = [](cmd_opt_t &cmd_opt) -> void * {
        std::string named;
        std::string kill_timeout_str = "3";
        std::string run_as;
        std::string depends_on_str;
        std::string start_timeout_str;
        std::string failure_action_str;
        std::string heartbeat_timeout_str;
        std::vector<std::string> env_vars;

        std::vector<std::string> depends_on;
        int start_timeout = 0;
        pm_tiny::failure_action_t failure_action = pm_tiny::failure_action_t::SKIP;
        int daemon = 1;
        int heartbeat_timeout = -1;
        std::vector<std::string *> options{&named, &kill_timeout_str,
                                           &run_as, &depends_on_str,
                                           &start_timeout_str, &failure_action_str,
                                           &heartbeat_timeout_str};
        int kill_timeout = 3;
        int option_index = 0;
        bool show_log = false;
        optind = 2;
        static struct option long_options[] = {
                {"name",              required_argument, nullptr, 0},
                {"kill_timeout",      required_argument, nullptr, 0},
                {"user",              required_argument, nullptr, 0},
                {"depends_on",        required_argument, nullptr, 0},
                {"start_timeout",     required_argument, nullptr, 0},
                {"failure_action",    required_argument, nullptr, 0},
                {"heartbeat_timeout", required_argument, nullptr, 0},
                {"no_daemon",         no_argument,       nullptr, 0},
                {"log",               no_argument,       nullptr, 0},
                {"env_var",           required_argument, nullptr, 0},
                {nullptr, 0,                             nullptr, 0}
        };
        auto uid = getuid();
        auto pass = getpwuid(uid);
        if (pass != nullptr) {
            run_as = pass->pw_name;
        } else {
            perror("getpwuid");
        }
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
                    if (option_index == 8) {
                        show_log = true;
                    } else if (option_index == 7) {
                        daemon = 0;
                    } else if (option_index == 9) {
                        env_vars.emplace_back(optarg);
                    } else {
                        *options[option_index] = optarg;
                    }
                    break;
                case '?':
                default:
                    exit(EXIT_FAILURE);
            }
        }

        kill_timeout = str2int(kill_timeout_str, 3);
        if (!depends_on_str.empty()) {
            depends_on = mgr::utils::split(depends_on_str, {','});
            std::for_each(depends_on.begin(), depends_on.end(), mgr::utils::trim);
            depends_on.erase(std::remove_if(depends_on.begin(), depends_on.end(),
                                            [](const std::string &x) { return x.empty(); }), depends_on.end());
        }
        if (!start_timeout_str.empty()) {
            start_timeout = str2int(start_timeout_str, 0);
        }
        if (!failure_action_str.empty()) {
            try {
                failure_action = pm_tiny::str_to_failure_action(failure_action_str);
            } catch (const std::exception &ex) {
                fprintf(stderr, PM_TINY_ANSI_COLOR_RED "%s" PM_TINY_ANSI_COLOR_REST "\n", ex.what());
                exit(EXIT_FAILURE);
            }
        }
        if (!heartbeat_timeout_str.empty()) {
            heartbeat_timeout = str2int(heartbeat_timeout_str, -1);
        }
        progcfg_t progcfg;
        progcfg.name = named;
        progcfg.kill_timeout_sec = kill_timeout;
        progcfg.run_as = run_as;
        progcfg.depends_on = std::move(depends_on);
        progcfg.start_timeout = start_timeout;
        progcfg.failure_action = failure_action;
        progcfg.daemon = daemon;
        progcfg.heartbeat_timeout = heartbeat_timeout;
        progcfg.command = cmd_opt.argv[1];
        progcfg.env_vars = env_vars;
        start_proc(*cmd_opt.session, progcfg, show_log);
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
        bool found = false;
        while (true) {
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
        int option_index = 0;
        bool show_log = false;
        optind = 2;
        static struct option long_options[] = {
                {"log",               no_argument,       nullptr, 0},
                {nullptr, 0,                             nullptr, 0}
        };
        int c;
        while (true) {
            c = getopt_long(cmd_opt.argc, cmd_opt.argv, "",
                            long_options, &option_index);
            if (c == -1)
                break;
            switch (c) {
                case 0:
                    if (option_index == 0) {
                        show_log = true;
                    }
                    break;
                case '?':
                default:
                    exit(EXIT_FAILURE);
            }
        }
        restart_prog(*cmd_opt.session, cmd_opt.argv[1],show_log);
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
            make_cmd_opt("inspect", inspect_fun, 1),
            make_cmd_opt("reload", reload_fun, 0),
            make_cmd_opt("quit", quit_fun, 0),

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
    auto sigint_handler = [](int) {
        pm_is_stop = 1;
    };
    if (pm_tiny::set_sigaction(SIGINT, sigint_handler) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_un serun{};
    int len;
    int sockfd;

    auto pm_tiny_cfg = pm_tiny::get_pm_tiny_config();
    auto sock_path = pm_tiny_cfg->pm_tiny_sock_file;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("client socket error");
        exit(1);
    }
    memset(&serun, 0, sizeof(serun));
    serun.sun_family = AF_UNIX;
    if (pm_tiny_cfg->uds_abstract_namespace) {
        serun.sun_path[0] = '\0';
        strncpy(serun.sun_path + 1, sock_path.c_str(), sizeof(serun.sun_path) - 2);
        len = offsetof(struct sockaddr_un, sun_path) + sock_path.length() + 1;
    } else {
        strncpy(serun.sun_path, sock_path.c_str(), sizeof(serun.sun_path) - 1);
        len = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
    }
    if (connect(sockfd, (struct sockaddr *) &serun, len) < 0) {
        perror("connect error");
        exit(1);
    }
    auto session = std::make_unique<pm_tiny::session_t>(sockfd, 0);
    cmd_opt->session = std::move(session);
    (*cmd_opt)();
    close(sockfd);
    return 0;
}