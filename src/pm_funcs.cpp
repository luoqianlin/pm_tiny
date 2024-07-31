//
// Created by qianlinluo@foxmail.com on 23-7-27.
//
#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <tabulate/table.hpp>
#include "pm_sys.h"
#include "pm_funcs.h"
#include "pm_tiny_enum.h"
#include "pm_tiny.h"
#include "ANSI_color.h"
#include "string_utils.h"
#include "memory_util.h"

extern sig_atomic_t pm_is_stop;
namespace pm_funcs {
    void show_msg(int code, const std::string &msg) {
        if (code != 0) {
            fprintf(stderr, "\033[31mFail(%d):%s\n\033[0m", code, msg.c_str());
        } else {
            printf("\033[32mSuccess\n\033[0m");
        }
    }


    void inspect_proc(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(PM_TINY_FRAME_TYPE_APP_INSPECT);
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
                return;
            }
            progcfg_t progcfg;
            progcfg.read(ifs);
            progcfg.show();
        }
    }

    void display_proc_infos(pm_tiny::session_t &session) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(0x23);
        session.write_frame(f, 1);
        if(session.is_close()){
            printf(PM_TINY_ANSI_COLOR_RED "Connection closed." PM_TINY_ANSI_COLOR_REST "\n");
            return;
        }
        auto rf = session.read_frame(1);
        constexpr const int max_field_length = 33;
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
                ifs >> proc_infos[i].vm_rss_kib;
                int daemon;
                ifs >> daemon;
                proc_infos[i].daemon = daemon != 0;
                int depends_size;
                ifs >> depends_size;
                auto &depends_on = proc_infos[i].depends_on;
                depends_on.resize(depends_size);
                for (int k = 0; k < depends_size; k++) {
                    ifs >> depends_on[k];
                }
            }
            std::vector<std::string> heads = {"pid", "name", "cwd",
                                              "command", "restart", "state", "memory", "daemon", "depends_on"};
            printf("Total:%zu\n", proc_infos.size());
            using namespace tabulate;
            Table prog_table;
            prog_table.add_row(Table::Row_t(std::begin(heads), std::end(heads)));
            auto truncate_long_str = [](const std::string &str, int maxlen) -> std::string {
                if (str.length() > static_cast<std::string::size_type>(maxlen) && maxlen > 3) {
                    return "..." + str.substr(str.length() - maxlen + 3);
                } else {
                    return str;
                }
            };

            for (auto &p: proc_infos) {
                std::string s_state = pm_state_to_str(p.state);
                std::string mem_size_str = pm_tiny::utils::memory::to_human_readable_size(p.vm_rss_kib);
                auto &depends_on = p.depends_on;
                auto depends_on_str = std::accumulate(depends_on.cbegin(), depends_on.cend(), std::string{},
                                                      [](auto &init, auto &s) {
                                                          return init + s + ",";
                                                      });
                if (!depends_on_str.empty()) {
                    depends_on_str.erase(std::prev(depends_on_str.end()));
                }
                if (depends_on_str.length() > 20) {
                    depends_on_str = truncate_long_str(depends_on_str, 20);
                }
                auto work_dir_str = p.work_dir;
                if (work_dir_str.length() > max_field_length) {
                    work_dir_str = truncate_long_str(work_dir_str,max_field_length);
                }
                auto command_str = p.command;
                if (command_str.length() > max_field_length) {
                    command_str =truncate_long_str(command_str,max_field_length);
                }
                Table::Row_t row{std::to_string(p.pid), p.name, work_dir_str,
                                 command_str, std::to_string(p.restart_count), s_state,
                                 mem_size_str, p.daemon ? "Y" : "N", depends_on_str};
                prog_table.add_row(row);
            }

            for (size_t i = 0; i < heads.size(); ++i) {
                prog_table[0][i].format()
                        .font_color(Color::cyan)
                        .font_align(FontAlign::center)
                        .font_style({FontStyle::bold});
            }
            for (size_t i = 0; i < proc_infos.size(); i++) {
                auto &p = proc_infos[i];
                if (p.state == PM_TINY_PROG_STATE_RUNING) {
                    prog_table[i + 1][5].format()
                            .font_color(tabulate::Color::green)
                            .font_style({tabulate::FontStyle::bold});
                } else if (p.state == PM_TINY_PROG_STATE_WAITING_START) {
                    prog_table[i + 1][5].format()
                            .font_color(tabulate::Color::none)
                            .font_style({tabulate::FontStyle::bold});
                } else if (p.state == PM_TINY_PROG_STATE_STARTING) {
                    prog_table[i + 1][5].format()
                            .font_color(tabulate::Color::blue)
                            .font_style({tabulate::FontStyle::bold});
                } else {
                    if (p.state == PM_TINY_PROG_STATE_EXIT && !p.daemon) {
                        prog_table[i + 1][5].format()
                                .font_color(tabulate::Color::blue)
                                .font_style({tabulate::FontStyle::bold});
                    } else {
                        prog_table[i + 1][5].format()
                                .font_color(tabulate::Color::red)
                                .font_style({tabulate::FontStyle::bold});
                    }
                }
                prog_table[i + 1][6].format().font_align(FontAlign::right);
                prog_table[i + 1][7].format().font_align(FontAlign::center);
            }
            std::cout << prog_table << std::endl;
        } else {
            printf("no data read\n");
        }
    }

    void stop_proc(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
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
                    const progcfg_t &prog_cfg, bool show_log) {
        std::vector<std::string> args;
        mgr::utils::split(prog_cfg.command, {' ', '\t'}, std::back_inserter(args));
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
            struct stat sb{};
            if (stat(app_realpath, &sb) == 0
                && (S_ISREG(sb.st_mode) && (sb.st_mode & S_IXUSR))) {
                exe_path = app_realpath;
                args[0] = exe_path;
                local_resolved = 1;
            }
        }
        std::string command = std::accumulate(args.begin(), args.end(), std::string(""),
                                              [](const std::string &s1, const std::string &s2) {
                                                  return s1 + (s2 + " ");
                                              });
        mgr::utils::trim(command);
        std::string filename = exe_path;
        std::string ext_name;
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
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(0x25);
        if (prog_cfg.name.empty()) {
            pm_tiny::fappend_value(*f, name);
        } else {
            pm_tiny::fappend_value(*f, prog_cfg.name);
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
        pm_tiny::fappend_value(*f, prog_cfg.kill_timeout_sec);
        pm_tiny::fappend_value(*f, prog_cfg.run_as);
        pm_tiny::fappend_value<int>(*f, show_log ? 1 : 0);
        pm_tiny::fappend_value(*f, static_cast<int>(prog_cfg.depends_on.size()));
        for (const auto &dep: prog_cfg.depends_on) {
            pm_tiny::fappend_value(*f, dep);
        }
        pm_tiny::fappend_value(*f, prog_cfg.start_timeout);
        pm_tiny::fappend_value(*f, static_cast<pm_tiny::failure_action_underlying_t>(prog_cfg.failure_action));
        pm_tiny::fappend_value(*f, prog_cfg.daemon);
        pm_tiny::fappend_value(*f, prog_cfg.heartbeat_timeout);
        pm_tiny::fappend_value(*f, prog_cfg.oom_score_adj);
        pm_tiny::fappend_value(*f, static_cast<int>(prog_cfg.env_vars.size()));
        std::for_each(prog_cfg.env_vars.begin(), prog_cfg.env_vars.end(),
                      [&](const auto &env_var) {
                          pm_tiny::fappend_value(*f, env_var);
                      });
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
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
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


    void delete_prog(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
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


    void restart_prog(pm_tiny::session_t &session, const std::string &app_name
                      ,bool show_log) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(0x28);
        pm_tiny::fappend_value(*f, app_name);
        pm_tiny::fappend_value(*f, show_log ? 1 : 0);
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

    void show_version(pm_tiny::session_t &session) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(0x29);
        session.write_frame(f, 1);
        auto rf = session.read_frame(1);

        if (rf) {
            pm_tiny::iframe_stream ifs(*rf);
            std::string version;
            ifs >> version;
            fprintf(stdout, "pm: %s\n", PM_TINY_VERSION);
            fprintf(stdout, "pm_tiny: %s\n", version.c_str());
        }
    }

    void loop_read_show_process_log(pm_tiny::session_t &session) {
        int msg_type = 0;
        std::string msg_content;
        int fd = session.get_fd();
        fd_set rfds;
        int rc;
        while (!pm_is_stop) {
            if (session.is_close()) {
                break;
            }
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            rc = select(fd + 1, &rfds, nullptr, nullptr, nullptr);
            if (rc == -1) {
                if (errno == EINTR) {
                    continue;
                } else {
                    perror("select");
                    exit(EXIT_FAILURE);
                }
            }
            session.read();
            msg_type = 1;
            while (true) {
                auto rf = session.get_frame_from_buf();
                if (!rf)break;
                pm_tiny::iframe_stream ifs(*rf);
                ifs >> msg_type;
                std::string msg_content_tmp;
                ifs >> msg_content_tmp;
                msg_content += msg_content_tmp;
                if (msg_type == 2) {
                    continue;
                }
                printf("%s", msg_content.c_str());
                fflush(stdout);
                msg_content = "";
                if (msg_type == 0) {
                    break;
                }
            }
            if (msg_type == 0) {
                break;
            }
        }
        printf("%s", PM_TINY_ANSI_COLOR_REST);
        fflush(stdout);
    }

    void show_prog_log(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
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

    void pm_tiny_reload(pm_tiny::session_t &session, int) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(PM_TINY_FRAME_TYPE_RELOAD);
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

    void pm_tiny_quit(pm_tiny::session_t &session) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(PM_TINY_FRAME_TYPE_QUIT);
        session.write_frame(f, 1);
        auto rf = session.read_frame(1);
        if (rf) {
            pm_tiny::iframe_stream ifs(*rf);
            int pid;
            int code = 0;
            std::string msg;
            ifs >> code;
            ifs >> msg;
            ifs >> pid;
            if (code == 0) {
                printf("Wait for the pm_tiny process to exit");
                fflush(stdout);
                while (pm_tiny::is_process_exists(pid) && !pm_is_stop) {
                    fprintf(stdout, ".");
                    fflush(stdout);
                    sleep(1);
                }
                printf("\n");
            }
            if (!pm_is_stop) {
                show_msg(code, msg);
            }
        }
    }

    void progcfg_t::read(pm_tiny::iframe_stream &ifs) {
        std::underlying_type_t<pm_tiny::failure_action_t> failure_action_v;
        ifs >> name;
        ifs >> work_dir;
        ifs >> command;
        ifs >> daemon;
        int depends_num;
        ifs >> depends_num;
        depends_on.resize(depends_num);
        for (int i = 0; i < depends_num; i++) {
            ifs >> depends_on[i];
        }
        ifs >> start_timeout;
        ifs >> failure_action_v;
        failure_action = static_cast<pm_tiny::failure_action_t>(failure_action_v);
        ifs >> heartbeat_timeout;
        ifs >> kill_timeout_sec;
        ifs >> run_as;
        ifs >> oom_score_adj;
    }

    void progcfg_t::show() {
        using namespace tabulate;
        Table prog_table;
        prog_table.add_row({"name", name});
        prog_table.add_row({"cwd", work_dir});
        prog_table.add_row({"command", command});
        prog_table.add_row({"user", run_as});
        prog_table.add_row({"daemon", daemon ? "Y" : "N"});
        std::string depends_on_ss;
        if (!depends_on.empty()) {
            for (size_t i = 0; i < depends_on.size(); i++) {
                depends_on_ss += depends_on[i];
                if (i != depends_on.size() - 1) {
                    depends_on_ss += ",";
                }
            }
        }
        prog_table.add_row({"depends_on", depends_on_ss});
        std::string start_timeout_ss;
        if (start_timeout > 0) {
            start_timeout_ss = std::to_string(start_timeout) + "s";
        } else if (start_timeout == 0) {
            start_timeout_ss = "available immediately";
        } else if (start_timeout < 0) {
            start_timeout_ss = "wait for external trigger";
        }
        prog_table.add_row({"start_timeout", start_timeout_ss});
        prog_table.add_row({"failure_action", pm_tiny::failure_action_to_str(failure_action)});
        std::string heartbeat_timeout_ss;
        if (heartbeat_timeout <= 0) {
            heartbeat_timeout_ss = "disable";
        } else {
            heartbeat_timeout_ss = std::to_string(heartbeat_timeout) + "s";
        }
        prog_table.add_row({"heartbeat_timeout", heartbeat_timeout_ss});
        prog_table.add_row({"kill_timeout", std::to_string(kill_timeout_sec) + "s"});
#ifdef __ANDROID__
        prog_table.add_row({"oom_score_adj", std::to_string(oom_score_adj)});
#endif
        std::cout << prog_table << std::endl;
    }
}