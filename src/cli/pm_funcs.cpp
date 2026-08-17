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
#include "cli_response.h"

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

        bool handle_response(const pm_tiny::cli::parsed_command &command,
                             const pm_tiny::frame_t &frame,
                             pm_tiny::cli::cli_response_result &result,
                             const pm_tiny::cli::list_render_options *list_options = nullptr,
                             const pm_tiny::cli::dependency_graph_render_options *graph_options = nullptr) {
            try {
                result = pm_tiny::cli::interpret_control_response(
                    command, frame, PM_TINY_VERSION, list_options, graph_options);
                if (!result.stdout_text.empty()) std::fputs(result.stdout_text.c_str(), stdout);
                if (!result.stderr_text.empty()) std::fputs(result.stderr_text.c_str(), stderr);
                return result.success;
            } catch (const std::exception &error) {
                std::fprintf(stderr, "pm: invalid response from pm_tiny: %s\n", error.what());
                return false;
            }
        }

        pm_tiny::cli::parsed_command named_command(pm_tiny::cli::command_kind kind,
                                                    const std::string &name = {}) {
            pm_tiny::cli::parsed_command command;
            command.kind = kind;
            command.name = name;
            return command;
        }
    }

    bool inspect_proc(pm_tiny::session_t &session, const std::string &app_name) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::inspect)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
        if (!rf) return false;
        pm_tiny::cli::cli_response_result result;
        return handle_response(named_command(pm_tiny::cli::command_kind::inspect, app_name), *rf, result);
    }

    bool display_daemon_info(pm_tiny::session_t &session, bool json) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
            pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::info)));
        auto response = request(session, f);
        if (!response) return false;
        auto command = named_command(pm_tiny::cli::command_kind::info);
        command.info_json = json;
        pm_tiny::cli::cli_response_result result;
        return handle_response(command, *response, result);
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
            pm_tiny::cli::cli_response_result result;
            return handle_response(named_command(pm_tiny::cli::command_kind::list), *rf, result,
                                   &effective_options);
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
        pm_tiny::cli::cli_response_result result;
        return handle_response(named_command(pm_tiny::cli::command_kind::graph), *rf, result,
                               nullptr, &options);
    }

    bool stop_proc(pm_tiny::session_t &session, const std::string &app_name, bool no_list) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::stop)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
        if (!rf) return false;
        auto command = named_command(pm_tiny::cli::command_kind::stop, app_name);
        command.no_list = no_list;
        pm_tiny::cli::cli_response_result result;
        if (!handle_response(command, *rf, result)) return false;
        return !result.post_list || display_proc_infos(session);
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
        auto command = named_command(pm_tiny::cli::command_kind::start);
        command.start.name = start_request.name;
        command.start.create = start_request.mode == pm_tiny::start_mode::create;
        command.start.show_log = start_request.show_log;
        pm_tiny::cli::cli_response_result result;
        if (!handle_response(command, *rf, result)) return false;
        return !result.stream_expected || loop_read_show_process_log(session);
    }


    bool save_proc(pm_tiny::session_t &session) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::save)));
        auto rf = request(session, f);

        if (!rf) return false;
        pm_tiny::cli::cli_response_result result;
        return handle_response(named_command(pm_tiny::cli::command_kind::save), *rf, result);
    }


    bool delete_prog(pm_tiny::session_t &session, const std::string &app_name, bool no_list) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::remove)));
        pm_tiny::fappend_value(*f, app_name);
        auto rf = request(session, f);
        if (!rf) return false;
        auto command = named_command(pm_tiny::cli::command_kind::remove, app_name);
        command.no_list = no_list;
        pm_tiny::cli::cli_response_result result;
        if (!handle_response(command, *rf, result)) return false;
        return !result.post_list || display_proc_infos(session);
    }


    bool restart_prog(pm_tiny::session_t &session, const std::string &app_name
                      ,bool show_log, bool no_list) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::restart)));
        pm_tiny::fappend_value(*f, app_name);
        pm_tiny::fappend_value(*f, show_log ? 1 : 0);
        auto rf = request(session, f);
        if (!rf) return false;
        auto command = named_command(pm_tiny::cli::command_kind::restart, app_name);
        command.show_log = show_log;
        command.no_list = no_list;
        pm_tiny::cli::cli_response_result result;
        if (!handle_response(command, *rf, result)) return false;
        if (result.stream_expected) return loop_read_show_process_log(session);
        return !result.post_list || display_proc_infos(session);
    }

    bool show_version(pm_tiny::session_t &session) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::version)));
        auto rf = request(session, f);

        if (!rf) return false;
        pm_tiny::cli::cli_response_result result;
        return handle_response(named_command(pm_tiny::cli::command_kind::version), *rf, result);
    }

    bool loop_read_show_process_log(pm_tiny::session_t &session) {
        int msg_type = 1;
        std::string msg_content;
        while (!pm_is_stop) {
            if (session.is_close()) {
                break;
            }
            bool consumed_frame = false;
            while (true) {
                auto rf = session.get_frame_from_buf();
                if (!rf)break;
                consumed_frame = true;
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
            if (consumed_frame) continue;
            if (wait_socket(session.get_fd(), false) != wait_result_t::ready) break;
            session.read();
        }
        return !pm_is_stop && msg_type == 0;
    }

    bool show_prog_log(pm_tiny::session_t &session, const std::string &app_name, bool history) {
        pm_tiny::frame_ptr_t f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::log)));
        pm_tiny::program_log_request log_request;
        log_request.name = app_name;
        log_request.mode = history ? pm_tiny::log_request_mode::history : pm_tiny::log_request_mode::live;
        std::vector<std::uint8_t> payload;
        pm_tiny::append_program_log_request(payload, log_request);
        f->insert(f->end(), payload.begin(), payload.end());
        auto rf = request(session, f);
        if (!rf) return false;
        pm_tiny::cli::cli_response_result result;
        auto command = named_command(pm_tiny::cli::command_kind::log, app_name);
        command.log_history = history;
        if (!handle_response(command, *rf, result))
            return false;
        return result.stream_expected && loop_read_show_process_log(session);
    }

    bool pm_tiny_reload(pm_tiny::session_t &session, int, bool no_list) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::reload)));
        auto rf = request(session, f);
        if (!rf) return false;
        auto command = named_command(pm_tiny::cli::command_kind::reload);
        command.no_list = no_list;
        pm_tiny::cli::cli_response_result result;
        if (!handle_response(command, *rf, result)) return false;
        return !result.post_list || display_proc_infos(session);
    }

    bool pm_tiny_quit(pm_tiny::session_t &session) {
        auto f = std::make_unique<pm_tiny::frame_t>();
        f->push_back(static_cast<std::uint8_t>(
                pm_tiny::cli::command_protocol_type(pm_tiny::cli::command_kind::quit)));
        auto rf = request(session, f);
        if (rf) {
            pm_tiny::cli::cli_response_result result;
            if (!handle_response(named_command(pm_tiny::cli::command_kind::quit), *rf, result)) return false;
            const int pid = static_cast<int>(result.daemon_pid);
            if (result.success) {
                constexpr int quit_timeout_seconds = 30;
                const auto wait_result = pm_tiny::wait_for_process_exit(
                        pid, quit_timeout_seconds * 1000, 100,
                        []() { return pm_is_stop != 0; });
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
            return result.success;
        }
        return false;
    }

}
