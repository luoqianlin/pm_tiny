#include <unistd.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <cerrno>
#include <iostream>
#include <fort.hpp>
#include "pm_sys.h"
#include "pm_funcs.h"
#include "pm_tiny_enum.h"
#include "pm_tiny.h"
#include "ANSI_color.h"
#include "log.h"
#include "string_utils.h"
#include "process_list.h"
#include "process_list_renderer.h"
#include "dependency_graph_renderer.h"
#include "cli_command.h"

extern volatile sig_atomic_t pm_is_stop;
namespace pm_funcs {
    namespace {
        enum class wait_result_t { ready, interrupted, error };

        wait_result_t wait_socket(int fd, bool write_ready) {
            if (fd < 0 || fd >= FD_SETSIZE) {
                errno = EBADF;
                perror("pselect");
                return wait_result_t::error;
            }
            sigset_t blocked_signals;
            sigset_t previous_signals;
            sigemptyset(&blocked_signals);
            sigaddset(&blocked_signals, SIGINT);
            if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_signals) == -1) {
                perror("sigprocmask");
                return wait_result_t::error;
            }
            wait_result_t result = wait_result_t::error;
            while (!pm_is_stop) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                int rc = ::pselect(fd + 1,
                                   write_ready ? nullptr : &fds,
                                   write_ready ? &fds : nullptr,
                                   nullptr, nullptr, &previous_signals);
                if (rc > 0 && FD_ISSET(fd, &fds)) {
                    result = wait_result_t::ready;
                    break;
                }
                if (rc < 0 && errno != EINTR) {
                    perror("pselect");
                    break;
                }
            }
            if (pm_is_stop) result = wait_result_t::interrupted;
            if (sigprocmask(SIG_SETMASK, &previous_signals, nullptr) == -1) {
                perror("sigprocmask");
                return wait_result_t::error;
            }
            return result;
        }

        bool write_command(pm_tiny::session_t &session,
                           const pm_tiny::frame_ptr_t &frame) {
            if (pm_is_stop || session.write_command_frame(frame, 0) < 0) return false;
            while (!session.sbuf_empty() && !session.is_close()) {
                if (wait_socket(session.get_fd(), true) != wait_result_t::ready) return false;
                session.write();
            }
            return !pm_is_stop && !session.is_close();
        }

        pm_tiny::frame_ptr_t read_response(pm_tiny::session_t &session) {
            while (!pm_is_stop && !session.is_close()) {
                auto frame = session.read_frame(0);
                if (frame) return frame;
                if (wait_socket(session.get_fd(), false) != wait_result_t::ready) break;
                session.read();
            }
            return nullptr;
        }

        pm_tiny::frame_ptr_t request(pm_tiny::session_t &session,
                                     const pm_tiny::frame_ptr_t &frame) {
            if (!write_command(session, frame)) return nullptr;
            return read_response(session);
        }
    }

    void show_msg(int code, const std::string &msg) {
        if (code != 0) {
            fprintf(stderr, "\033[31mFail(%d):%s\n\033[0m", code, msg.c_str());
        } else {
            printf("\033[32mSuccess\n\033[0m");
        }
    }


    void inspect_proc(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::inspect)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
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

    void display_proc_infos(pm_tiny::session_t &session,
                            const pm_tiny::cli::list_render_options &options) {
        auto effective_options = options;
        if (effective_options.terminal_width == 0) {
            effective_options.terminal_width = pm_tiny::cli::stdout_terminal_width();
            effective_options.stdout_is_tty = pm_tiny::cli::stdout_supports_color();
        }
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::list)));
        auto rf = request(session, f);
        if(session.is_close()){
            printf(PM_TINY_ANSI_COLOR_RED "Connection closed." PM_TINY_ANSI_COLOR_REST "\n");
            return;
        }
        if (rf) {
            try {
                pm_tiny::iframe_stream ifs(*rf);
                int code = 0;
                std::string msg;
                ifs >> code >> msg;
                if (code != 0) { show_msg(code, msg); return; }
                const auto entries = pm_tiny::read_process_list(ifs);
                std::cout << pm_tiny::cli::render_process_list(entries, effective_options);
            } catch (const std::exception &error) {
                fprintf(stderr, "Invalid process-list response: %s\n", error.what());
            }
        } else if (!pm_is_stop) {
            printf("no data read\n");
        }
    }

    bool display_dependency_graph(pm_tiny::session_t &session,
                                  const pm_tiny::cli::dependency_graph_render_options &options) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::graph)));
        auto rf = request(session, f);
        if (session.is_close()) {
            fprintf(stderr, "Connection closed.\n");
            return false;
        }
        if (!rf) {
            if (!pm_is_stop) fprintf(stderr, "no data read\n");
            return false;
        }
        try {
            pm_tiny::iframe_stream ifs(*rf);
            int code = 0;
            std::string msg;
            ifs >> code >> msg;
            if (code != 0) {
                show_msg(code, msg);
                return false;
            }
            std::cout << pm_tiny::cli::render_dependency_graph(pm_tiny::read_process_list(ifs), options);
            return true;
        } catch (const std::exception &error) {
            fprintf(stderr, "Cannot render dependency graph: %s\n", error.what());
            return false;
        }
    }

    void stop_proc(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::stop)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::start)));
        if (prog_cfg.name.empty()) {
            pm_tiny::fappend_value(*f, name);
        } else {
            pm_tiny::fappend_value(*f, prog_cfg.name);
        }
        char cwd[PATH_MAX];
        getcwd(cwd, sizeof(cwd));
        if (command.find(cwd) == 0 && strcmp(cwd, "/") != 0) {
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
        pm_tiny::fappend_value<int>(*f, prog_cfg.pty ? 1 : 0);
        auto rf = request(session, f);
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::save)));
        auto rf = request(session, f);

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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::remove)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::restart)));
        pm_tiny::fappend_value(*f, app_name);
        pm_tiny::fappend_value(*f, show_log ? 1 : 0);
        auto rf = request(session, f);
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::version)));
        auto rf = request(session, f);

        if (rf) {
            pm_tiny::iframe_stream ifs(*rf);
            int code = 0;
            std::string msg;
            ifs >> code >> msg;
            std::string version;
            ifs >> version;
            if (code == 0) {
                fprintf(stdout, "pm: %s\n", PM_TINY_VERSION);
                fprintf(stdout, "pm_tiny: %s\n", version.c_str());
            } else {
                show_msg(code, msg);
            }
        }
    }

    void loop_read_show_process_log(pm_tiny::session_t &session) {
        int msg_type = 0;
        std::string msg_content;
        while (!pm_is_stop) {
            if (session.is_close()) {
                break;
            }
            if (wait_socket(session.get_fd(), false) != wait_result_t::ready) break;
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::log)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
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
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::reload)));
        auto rf = request(session, f);
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

    bool pm_tiny_quit(pm_tiny::session_t &session) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::quit)));
        auto rf = request(session, f);
        if (rf) {
            pm_tiny::iframe_stream ifs(*rf);
            int pid;
            int code = 0;
            std::string msg;
            ifs >> code;
            ifs >> msg;
            ifs >> pid;
            if (code == 0) {
                constexpr int quit_timeout_seconds = 30;
                printf("Waiting up to %d seconds for pm_tiny (pid %d) to exit", quit_timeout_seconds, pid);
                fflush(stdout);
                const auto wait_result = pm_tiny::wait_for_process_exit(
                        pid, quit_timeout_seconds * 1000, 100,
                        []() { return pm_is_stop != 0; });
                printf("\n");
                if (wait_result == pm_tiny::process_wait_result::timed_out) {
                    fprintf(stderr,
                            "pm: timed out after %d seconds waiting for pm_tiny (pid %d) to exit\n"
                            "  hint: inspect the daemon process state and pm_tiny log for a blocked shutdown.\n",
                            quit_timeout_seconds, pid);
                    return false;
                }
                if (wait_result == pm_tiny::process_wait_result::interrupted) {
                    fprintf(stderr, "pm: interrupted while waiting for pm_tiny (pid %d) to exit\n", pid);
                    return false;
                }
            }
            if (!pm_is_stop) {
                show_msg(code, msg);
            }
            return code == 0;
        }
        return false;
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
        int pty_flag = 1;
        ifs >> pty_flag;
        pty = (pty_flag != 0);
        int restart_pending_flag = 0;
        int restart_suppressed_flag = 0;
        ifs >> restart_delay_ms >> restart_max_delay_ms >> restart_window_ms
            >> restart_max_attempts >> restart_reset_after_ms;
        ifs >> restart_pending_flag >> restart_delay_remaining_ms
            >> restart_attempts_in_window >> restart_suppressed_flag
            >> restart_suppression_reason;
        restart_pending = restart_pending_flag != 0;
        restart_suppressed = restart_suppressed_flag != 0;
    }

    void progcfg_t::show() {
        fort::utf8_table prog_table;
        prog_table.set_border_style(FT_BASIC_STYLE);
        prog_table << "name" << name << fort::endr;
        prog_table << "cwd" << work_dir << fort::endr;
        prog_table << "command" << command << fort::endr;
        prog_table << "user" << run_as << fort::endr;
        prog_table << "daemon" << (daemon ? "Y" : "N") << fort::endr;
        prog_table << "pty" << (pty ? "Y" : "N") << fort::endr;
        std::string depends_on_ss;
        if (!depends_on.empty()) {
            for (size_t i = 0; i < depends_on.size(); i++) {
                depends_on_ss += depends_on[i];
                if (i != depends_on.size() - 1) {
                    depends_on_ss += ",";
                }
            }
        }
        prog_table << "depends_on" << depends_on_ss << fort::endr;
        std::string start_timeout_ss;
        if (start_timeout > 0) {
            start_timeout_ss = std::to_string(start_timeout) + "s";
        } else if (start_timeout == 0) {
            start_timeout_ss = "available immediately";
        } else if (start_timeout < 0) {
            start_timeout_ss = "wait for external trigger";
        }
        prog_table << "start_timeout" << start_timeout_ss << fort::endr;
        prog_table << "failure_action" << pm_tiny::failure_action_to_str(failure_action) << fort::endr;
        std::string heartbeat_timeout_ss;
        if (heartbeat_timeout <= 0) {
            heartbeat_timeout_ss = "disable";
        } else {
            heartbeat_timeout_ss = std::to_string(heartbeat_timeout) + "s";
        }
        prog_table << "heartbeat_timeout" << heartbeat_timeout_ss << fort::endr;
        prog_table << "kill_timeout" << std::to_string(kill_timeout_sec) + "s" << fort::endr;
        prog_table << "restart_delay_ms" << restart_delay_ms << fort::endr;
        prog_table << "restart_max_delay_ms" << restart_max_delay_ms << fort::endr;
        prog_table << "restart_window_ms" << restart_window_ms << fort::endr;
        prog_table << "restart_max_attempts" << restart_max_attempts << fort::endr;
        prog_table << "restart_reset_after_ms" << restart_reset_after_ms << fort::endr;
        prog_table << "restart_pending" << (restart_pending ? "Y" : "N") << fort::endr;
        prog_table << "restart_delay_remaining_ms"
                   << (restart_pending ? std::to_string(restart_delay_remaining_ms) : "-") << fort::endr;
        prog_table << "restart_attempts_in_window" << restart_attempts_in_window << fort::endr;
        prog_table << "restart_suppressed" << (restart_suppressed ? "Y" : "N") << fort::endr;
        prog_table << "restart_suppression_reason"
                   << (restart_suppressed ? restart_suppression_reason : "-") << fort::endr;
#ifdef __ANDROID__
        prog_table << "oom_score_adj" << std::to_string(oom_score_adj) << fort::endr;
#endif
        std::cout << prog_table.to_string() << std::endl;
    }
}
