//
// Created by qianlinluo@foxmail.com on 23-7-27.
//
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
#include "daemon_log.h"
#include "string_utils.h"
#include "process_list.h"
#include "process_list_renderer.h"
#include "daemon_info.h"
#include "daemon_info_renderer.h"
#include "inspect_renderer.h"
#include "runtime_snapshot.h"
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


    bool inspect_proc(pm_tiny::session_t &session, const std::string &app_name) {
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
                return false;
            }
            const auto snapshot = pm_tiny::read_inspect_snapshot(ifs);
            std::cout << pm_tiny::cli::render_inspect_snapshot(snapshot);
            return true;
        }
        return false;
    }

    bool display_daemon_info(pm_tiny::session_t &session, bool json) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
            pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::info)));
        auto response = request(session, f);
        if (!response) return false;
        try {
            pm_tiny::iframe_stream stream(*response);
            std::int32_t status = -1;
            std::string message;
            stream >> status >> message;
            if (status != 0) { show_msg(status, message); return false; }
            std::cout << pm_tiny::cli::render_daemon_info(pm_tiny::read_daemon_info(stream), json);
            return true;
        } catch (const std::exception &error) {
            fprintf(stderr, "Invalid daemon-info response: %s\n", error.what());
            return false;
        }
    }

    bool display_proc_infos(pm_tiny::session_t &session,
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
            return false;
        }
        if (rf) {
            try {
                pm_tiny::iframe_stream ifs(*rf);
                int code = 0;
                std::string msg;
                ifs >> code >> msg;
                if (code != 0) { show_msg(code, msg); return false; }
                const auto entries = pm_tiny::read_process_list(ifs);
                std::cout << pm_tiny::cli::render_process_list(entries, effective_options);
                return true;
            } catch (const std::exception &error) {
                fprintf(stderr, "Invalid process-list response: %s\n", error.what());
                return false;
            }
        } else if (!pm_is_stop) {
            printf("no data read\n");
        }
        return false;
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

    bool stop_proc(pm_tiny::session_t &session, const std::string &app_name) {
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
            if (code == 0) display_proc_infos(session);
            return code == 0;
        }
        return false;
    }


    bool start_proc(pm_tiny::session_t &session, const pm_tiny::start_request &start_request) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::start)));
        std::vector<std::uint8_t> payload;
        pm_tiny::append_start_request(payload, start_request);
        f->insert(f->end(), payload.begin(), payload.end());
        auto rf = request(session, f);
        if (!rf) return false;
        pm_tiny::iframe_stream ifs(*rf);
        int code = 0;
        std::string msg;
        ifs >> code >> msg;
        if (code != 0) {
            show_msg(code, msg);
            return false;
        }
        const auto response = pm_tiny::read_start_response(ifs.remaining_frame());
        if (response.result == pm_tiny::start_result::started) {
            std::printf("started `%s` pid=%lld%s\n", start_request.name.c_str(),
                        static_cast<long long>(response.pid),
                        start_request.mode == pm_tiny::start_mode::create ? "; run `pm save` to persist" : "");
        } else if (response.result == pm_tiny::start_result::waiting) {
            std::printf("waiting `%s`", start_request.name.c_str());
            if (!response.blocked_by.empty()) std::printf(" for: %s", mgr::utils::join(response.blocked_by, ",").c_str());
            std::printf("\n");
        } else {
            std::fprintf(stderr, "blocked `%s` by: %s\n", start_request.name.c_str(),
                         mgr::utils::join(response.blocked_by, ",").c_str());
            return false;
        }
        if (start_request.show_log && response.result != pm_tiny::start_result::blocked)
            return loop_read_show_process_log(session);
        return true;
    }


    bool save_proc(pm_tiny::session_t &session) {
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
            return code == 0;
        }
        return false;
    }


    bool delete_prog(pm_tiny::session_t &session, const std::string &app_name) {
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
            if (code == 0) display_proc_infos(session);
            return code == 0;
        }
        return false;
    }


    bool restart_prog(pm_tiny::session_t &session, const std::string &app_name
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
                if (code == 0) display_proc_infos(session);
                return code == 0;
            } else {
                return loop_read_show_process_log(session);
            }
        }
        return false;
    }

    bool show_version(pm_tiny::session_t &session) {
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
                return true;
            } else {
                show_msg(code, msg);
            }
        }
        return false;
    }

    bool loop_read_show_process_log(pm_tiny::session_t &session) {
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
        return !pm_is_stop && msg_type == 0;
    }

    bool show_prog_log(pm_tiny::session_t &session, const std::string &app_name) {
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
                return false;
            } else {
                return loop_read_show_process_log(session);
            }
        }
        return false;
    }

    bool pm_tiny_reload(pm_tiny::session_t &session, int) {
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
            if (code == 0) display_proc_infos(session);
            return code == 0;
        }
        return false;
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

}
