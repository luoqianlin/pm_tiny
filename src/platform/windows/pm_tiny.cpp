#include "control_server.h"
#include "process_runner.h"
#include "win_config_loader.h"
#include "win_utils.h"
#include "daemon_log.h"
#include "daemon_config.h"
#include "pm_tiny.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace {

std::atomic_bool g_should_stop{false};

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_should_stop.store(true);
            return TRUE;
        case CTRL_BREAK_EVENT:
            return TRUE;
        default:
            return FALSE;
    }
}

void log_info(const std::string &message) {
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::info, message);
}

void log_error(const std::string &message) {
    pm_tiny::daemon_log_message(pm_tiny::daemon_log_level_t::error, message);
}

bool create_directory_tree(const std::string &path, std::string &error) {
    const int result = SHCreateDirectoryExW(nullptr, pm_tiny::win::utf8_to_wide(path).c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS)
        return true;
    error = "Cannot create directory `" + path + "`: " + std::to_string(result);
    return false;
}

unsigned long next_loop_wait_ms(const std::vector<pm_tiny::win::ProcessHandle> &processes,
                                unsigned long long now_ms, bool persistence_busy) {
    if (persistence_busy) return 10;
    unsigned long long next_due_ms = now_ms + 1000;
    bool draining = false;
    const auto consider = [&](unsigned long long due_ms) {
        if (due_ms <= now_ms) next_due_ms = now_ms;
        else next_due_ms = std::min(next_due_ms, due_ms);
    };
    for (const auto &process : processes) {
        if (!process.has_process) {
            if (process.restart_pending) consider(process.restart_due_ms);
            continue;
        }
        if (process.termination.phase() != pm_tiny::termination_phase::none) {
            draining = true;
            const auto phase = process.termination.phase();
            if (phase == pm_tiny::termination_phase::term_requested ||
                phase == pm_tiny::termination_phase::tree_draining)
                consider(static_cast<unsigned long long>(process.termination.deadline_ms()));
            continue;
        }
        if (!process.ready && process.config.start_timeout > 0) {
            consider(process.launch_time_ms +
                     static_cast<unsigned long long>(process.config.start_timeout) * 1000ULL);
        } else if (process.ready && process.config.heartbeat_timeout > 0) {
            consider(process.last_tick_ms +
                     static_cast<unsigned long long>(process.config.heartbeat_timeout) * 1000ULL);
        }
    }
    if (draining) return 25;
    if (next_due_ms <= now_ms) return 1;
    return static_cast<unsigned long>(std::min<unsigned long long>(1000, next_due_ms - now_ms));
}

pm_tiny::daemon_cli_options g_options;
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};

void report_service_status(DWORD state, DWORD win32_exit_code, DWORD wait_hint) {
    if (g_service_status_handle == nullptr) return;
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit_code;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
#ifdef SERVICE_ACCEPT_PRESHUTDOWN
    if (state == SERVICE_RUNNING) g_service_status.dwControlsAccepted |= SERVICE_ACCEPT_PRESHUTDOWN;
#endif
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

DWORD WINAPI service_ctrl_handler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
#ifdef SERVICE_CONTROL_PRESHUTDOWN
        case SERVICE_CONTROL_PRESHUTDOWN:
#endif
            if (!g_should_stop.exchange(true)) {
                report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            report_service_status(g_service_status.dwCurrentState,
                                  g_service_status.dwWin32ExitCode,
                                  g_service_status.dwWaitHint);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

int run_daemon(const pm_tiny::daemon_cli_options &options) {
    g_should_stop.store(false);
    const auto resolved = pm_tiny::resolve_daemon_config(options, pm_tiny::daemon_platform::windows);
    if (!resolved.success) {
        log_error(resolved.error);
        return EXIT_FAILURE;
    }
    const auto &daemon_config = resolved.config;
    std::string directory_error;
    if (!create_directory_tree(daemon_config.home_dir, directory_error) ||
        !create_directory_tree(daemon_config.app_log_dir, directory_error) ||
        !create_directory_tree(daemon_config.app_environ_dir, directory_error)) {
        log_error(directory_error);
        return EXIT_FAILURE;
    }
    std::string environment_error;
    const std::pair<const char *, std::string> exported[] = {
        {PM_TINY_HOME, daemon_config.home_dir},
        {PM_TINY_LOG_FILE, daemon_config.log_file},
        {PM_TINY_PROG_CFG_FILE, daemon_config.program_config_file},
        {PM_TINY_APP_LOG_DIR, daemon_config.app_log_dir},
        {PM_TINY_APP_ENVIRON_DIR, daemon_config.app_environ_dir},
        {PM_TINY_LOG_LEVEL, daemon_config.log_level},
        {PM_TINY_LOG_MAX_SIZE_KB, std::to_string(daemon_config.log_max_size_kb)},
        {PM_TINY_LOG_ARCHIVE_COUNT, std::to_string(daemon_config.log_archive_count)},
        {PM_TINY_PIPE_NAME, daemon_config.pipe_name},
        {PM_TINY_PIPE_SDDL, daemon_config.pipe_sddl}
    };
    for (const auto &entry : exported) {
        if (!pm_tiny::set_daemon_environment(entry.first, entry.second, environment_error)) {
            log_error(environment_error);
            return EXIT_FAILURE;
        }
    }
    pm_tiny::daemon_log_level_t minimum_level;
    if (!pm_tiny::parse_daemon_log_level(daemon_config.log_level, minimum_level)) {
        log_error("Invalid daemon log level: " + daemon_config.log_level);
        return EXIT_FAILURE;
    }
    pm_tiny::daemon_log_config_t log_config;
    log_config.path = daemon_config.log_file;
    log_config.max_size_bytes =
        static_cast<std::size_t>(daemon_config.log_max_size_kb) * 1024U;
    log_config.archive_count = daemon_config.log_archive_count;
    log_config.mirror_console = !options.service;
    log_config.minimum_level = minimum_level;
    std::string log_error_message;
    pm_tiny::configure_daemon_log(log_config, log_error_message);
    auto cfg_result = pm_tiny::win::load_program_configs(daemon_config.program_config_file,
                                                         daemon_config.app_environ_dir);
    if (!cfg_result.error_message.empty()) {
        log_error(cfg_result.error_message);
        return EXIT_FAILURE;
    }

    bool has_initial_programs = !cfg_result.programs.empty();
    if (!has_initial_programs) {
        log_info("No preconfigured programs; waiting for CLI commands.");
    } else {
        log_info("Loaded " + std::to_string(cfg_result.programs.size()) + " program entries.");
    }

    std::map<std::string, pm_tiny::win::ProgramConfig> config_map;
    for (const auto &cfg : cfg_result.programs) {
        config_map[cfg.name] = cfg;
    }

    std::vector<pm_tiny::win::ProcessHandle> processes;
    processes.reserve(cfg_result.programs.size());
    pm_tiny::win::RuntimeControlState runtime_state;
    std::string dependency_error;
    if (!pm_tiny::win::rebuild_dependencies(runtime_state, cfg_result.programs, dependency_error)) {
        log_error(dependency_error);
        return EXIT_FAILURE;
    }
    pm_tiny::win::ensure_process_records(processes, cfg_result.programs);

    pm_tiny::win::ControlServer control_server(processes, config_map, runtime_state,
                                               g_should_stop, daemon_config.program_config_file,
                                               daemon_config.app_environ_dir,
                                               daemon_config.app_log_dir,
                                               daemon_config, options,
                                               pm_tiny::win::monotonic_millis());
    if (!control_server.start()) {
        log_error("Failed to start control server.");
        return EXIT_FAILURE;
    }
    if (options.service) report_service_status(SERVICE_RUNNING, NO_ERROR, 0);

    if (has_initial_programs) {
        const auto failures = pm_tiny::win::schedule_dependency_launch(
            processes, runtime_state, runtime_state.dependencies.request_all());
        for (const auto &failure : failures) log_error("Failed to start program `" + failure);
    }

    bool shutdown_scheduled = false;
    while (true) {
        control_server.poll();
        bool shutdown_complete = false;
        {
            const auto now_ms = pm_tiny::win::monotonic_millis();

            if (g_should_stop.load() && !shutdown_scheduled) {
                log_info("Termination requested. Stopping child processes...");
                shutdown_scheduled = true;
                runtime_state.reload_pending = false;
                runtime_state.reload_programs.clear();
                for (const auto id : runtime_state.graph.reverse_topological_order()) {
                    auto iter = std::find_if(processes.begin(), processes.end(), [&](const auto &proc) {
                        return proc.config.name == runtime_state.graph.name(id);
                    });
                    if (iter == processes.end() || !iter->has_process) continue;
                    iter->disable_restart = true;
                    std::string error;
                    if (!pm_tiny::win::request_program_termination(
                            *iter, pm_tiny::win::CompletionAction::remove, 0, error)) {
                        log_error("Failed to stop `" + iter->config.name + "`: " + error);
                    } else if (!error.empty()) {
                        pm_tiny::win::write_stderr_utf8("[WARN] " + iter->config.name + ": " + error + "\n");
                    }
                }
            }

            for (auto it = processes.begin(); it != processes.end();) {
                auto &proc = *it;
                if (proc.pipe_read[0] != nullptr || proc.pipe_read[1] != nullptr)
                    pm_tiny::win::poll_program_log(proc);
                if (!proc.has_process) {
                    if (proc.restart_pending && !shutdown_scheduled && !runtime_state.reload_pending &&
                        now_ms >= proc.restart_due_ms) {
                        std::string launch_error;
                        if (pm_tiny::win::launch_program(proc, launch_error)) {
                            runtime_state.dependencies.mark_starting(proc.config.name);
                            if (proc.config.start_timeout == 0) {
                                proc.ready = true;
                                const auto failures = pm_tiny::win::schedule_dependency_launch(
                                    processes, runtime_state, runtime_state.dependencies.mark_ready(proc.config.name));
                                for (const auto &failure : failures) log_error("Failed to start program `" + failure);
                            }
                            log_info("Restarted program `" + proc.config.name + "` as generation " +
                                     std::to_string(proc.generation) + ".");
                            ++it;
                        } else {
                            log_error("Failed to restart program `" + proc.config.name + "`: " + launch_error);
                            runtime_state.dependencies.mark_failed(proc.config.name);
                            ++it;
                        }
                    } else {
                        ++it;
                    }
                    continue;
                }

                const DWORD root_exit_code = proc.root_exit_code;
                const bool root_status_ok = proc.root_exit_observed;
                const bool root_active = !proc.root_exit_observed;
                const bool start_timed_out = root_active && proc.termination.phase() == pm_tiny::termination_phase::none &&
                    proc.config.start_timeout > 0 && !proc.ready &&
                    now_ms - proc.launch_time_ms > static_cast<unsigned long long>(proc.config.start_timeout) * 1000ULL;
                const bool heartbeat_timed_out = root_active && proc.termination.phase() == pm_tiny::termination_phase::none &&
                    proc.config.heartbeat_timeout > 0 && proc.ready &&
                    now_ms - proc.last_tick_ms > static_cast<unsigned long long>(proc.config.heartbeat_timeout) * 1000ULL;
                if (start_timed_out || heartbeat_timed_out) {
                    log_error("Program `" + proc.config.name + "` " +
                              (start_timed_out ? "start timeout." : "heartbeat timeout."));
                    std::string terminate_error;
                    if (!pm_tiny::win::request_program_termination(
                            proc, pm_tiny::win::CompletionAction::automatic, 1, terminate_error)) {
                        log_error(terminate_error);
                    } else if (!terminate_error.empty()) {
                        pm_tiny::win::write_stderr_utf8("[WARN] " + proc.config.name + ": " + terminate_error + "\n");
                    }
                    if (start_timed_out) runtime_state.dependencies.mark_failed(proc.config.name);
                }

                bool tree_empty = false;
                std::string poll_error;
                const auto phase_before_poll = proc.termination.phase();
                if (!pm_tiny::win::poll_program_termination(proc, now_ms, tree_empty, poll_error)) {
                    log_error("Failed to poll `" + proc.config.name + "` generation " +
                              std::to_string(proc.generation) + ": " + poll_error);
                    ++it;
                    continue;
                }
                if ((phase_before_poll == pm_tiny::termination_phase::term_requested ||
                     phase_before_poll == pm_tiny::termination_phase::tree_draining) &&
                    proc.termination.phase() == pm_tiny::termination_phase::force_kill_requested) {
                    pm_tiny::win::write_stderr_utf8(
                        "[WARN] Program `" + proc.config.name + "` generation " +
                        std::to_string(proc.generation) +
                        " exceeded kill_timeout; forced Job Object termination.\n");
                }
                if (!root_active && !tree_empty && proc.termination.phase() == pm_tiny::termination_phase::none) {
                    std::string terminate_error;
                    if (!pm_tiny::win::request_program_termination(
                            proc, pm_tiny::win::CompletionAction::automatic, 1, terminate_error)) {
                        log_error(terminate_error);
                    } else {
                        log_info("Program `" + proc.config.name + "` generation " +
                                 std::to_string(proc.generation) + " exited while descendants remain; draining Job Object.");
                    }
                    ++it;
                    continue;
                }
                if (!tree_empty) {
                    ++it;
                    continue;
                }
                const bool logs_eof = proc.log_pipe_eof[0] &&
                    (proc.config.log_mode != pm_tiny::log_mode_t::split || proc.log_pipe_eof[1]);
                if (!logs_eof) {
                    ++it;
                    continue;
                }

                const auto completed_generation = proc.generation;
                const auto completion_action = proc.completion_action;
                const bool completed_ready = proc.ready;
                const auto completed_name = proc.config.name;
                const auto completed_runtime_ms = proc.launch_time_ms > 0 ? now_ms - proc.launch_time_ms : 0;
                const bool automatic_restart = completion_action == pm_tiny::win::CompletionAction::automatic &&
                                               proc.config.daemon && !proc.disable_restart;
                const bool should_restart = !shutdown_scheduled && !runtime_state.reload_pending &&
                    (completion_action == pm_tiny::win::CompletionAction::restart || automatic_restart);
                log_info("Program `" + proc.config.name + "` generation " +
                         std::to_string(completed_generation) + " exited with code " +
                         std::to_string(root_status_ok ? root_exit_code : static_cast<DWORD>(-1)) + ".");
                proc.reset();
                if (proc.generation != completed_generation) {
                    ++it;
                    continue;
                }
                if (should_restart) {
                    if (completion_action == pm_tiny::win::CompletionAction::restart) {
                        proc.restart_state.reset();
                        proc.restart_pending = true;
                        proc.restart_due_ms = now_ms;
                    } else {
                        pm_tiny::restart_policy_config config;
                        config.delay_ms = proc.config.restart_delay_ms;
                        config.max_delay_ms = proc.config.restart_max_delay_ms;
                        config.window_ms = proc.config.restart_window_ms;
                        config.max_attempts = proc.config.restart_max_attempts;
                        config.reset_after_ms = proc.config.restart_reset_after_ms;
                        const auto decision = pm_tiny::plan_automatic_restart(
                            config, proc.restart_state, now_ms, completed_runtime_ms);
                        proc.restart_pending = decision.restart;
                        proc.restart_due_ms = decision.restart
                            ? now_ms + static_cast<unsigned long long>(decision.delay_ms) : 0;
                        if (decision.restart) {
                            log_info("Program `" + proc.config.name + "` restart scheduled in " +
                                     std::to_string(decision.delay_ms) + "ms (attempt " +
                                     std::to_string(decision.attempts_in_window) + ").");
                        } else {
                            log_error("Program `" + proc.config.name + "` automatic restart suppressed after " +
                                      std::to_string(decision.attempts_in_window) + " attempts.");
                        }
                    }
                    ++it;
                } else {
                    if (!completed_ready && completion_action != pm_tiny::win::CompletionAction::remove &&
                        completion_action != pm_tiny::win::CompletionAction::delete_config)
                        runtime_state.dependencies.mark_failed(completed_name);
                    if (completion_action == pm_tiny::win::CompletionAction::remove) {
                        runtime_state.dependencies.mark_idle(completed_name);
                        ++it;
                    } else if (completion_action == pm_tiny::win::CompletionAction::delete_config) {
                        config_map.erase(completed_name);
                        it = processes.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            const bool all_processes_stopped = std::none_of(processes.begin(), processes.end(),
                [](const auto &proc) {
                    return proc.has_process || proc.termination.phase() != pm_tiny::termination_phase::none;
                });
            if (runtime_state.reload_pending && all_processes_stopped && !shutdown_scheduled) {
                config_map.clear();
                for (const auto &cfg : runtime_state.reload_programs) config_map[cfg.name] = cfg;
                processes.clear();
                std::string reload_dependency_error;
                if (!pm_tiny::win::rebuild_dependencies(runtime_state, runtime_state.reload_programs,
                                                         reload_dependency_error)) {
                    log_error(reload_dependency_error);
                } else {
                    pm_tiny::win::ensure_process_records(processes, runtime_state.reload_programs);
                    for (auto &process : processes) process.config_source = "file";
                    const auto failures = pm_tiny::win::schedule_dependency_launch(
                        processes, runtime_state, runtime_state.dependencies.request_all());
                    for (const auto &failure : failures) log_error("Failed to reload program `" + failure);
                }
                runtime_state.reload_programs.clear();
                runtime_state.reload_pending = false;
            }

            shutdown_complete = shutdown_scheduled && all_processes_stopped &&
                                !control_server.persistence_busy();
        }
        if (shutdown_complete) break;
        control_server.run_for(next_loop_wait_ms(processes, pm_tiny::win::monotonic_millis(),
                                                 control_server.persistence_busy()));
    }

    control_server.stop();
    log_info("pm_tiny Windows service exited.");

    return EXIT_SUCCESS;
}

void WINAPI service_main(DWORD, LPWSTR *) {
    const std::wstring service_name = pm_tiny::win::utf8_to_wide(g_options.service_name);
    g_service_status_handle = RegisterServiceCtrlHandlerExW(
        service_name.c_str(), service_ctrl_handler, nullptr);
    if (g_service_status_handle == nullptr) return;

    report_service_status(SERVICE_START_PENDING, NO_ERROR, 15000);
    const int result = run_daemon(g_options);
    report_service_status(SERVICE_STOPPED,
                          result == EXIT_SUCCESS ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR,
                          0);
}

} // namespace

int wmain(int argc, wchar_t *argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    try {
        for (int i = 0; i < argc; ++i) arguments.push_back(pm_tiny::win::wide_to_utf8(argv[i]));
    } catch (const std::exception &ex) {
        pm_tiny::win::write_stderr_utf8("pm_tiny: invalid command line: " + std::string(ex.what()) + "\n");
        return 2;
    }
    const auto parsed = pm_tiny::parse_daemon_arguments(arguments, pm_tiny::daemon_platform::windows);
    if (!parsed.success) {
        pm_tiny::win::write_stderr_utf8(
            "pm_tiny: " + parsed.error + "\n\n" +
            pm_tiny::daemon_usage(arguments.empty() ? "pm_tiny" : arguments[0],
                                  pm_tiny::daemon_platform::windows));
        return 2;
    }
    g_options = parsed.options;
    if (g_options.help) {
        pm_tiny::win::write_stdout_utf8(
            pm_tiny::daemon_usage(arguments.empty() ? "pm_tiny" : arguments[0],
                                  pm_tiny::daemon_platform::windows));
        return EXIT_SUCCESS;
    }
    if (g_options.version) {
        pm_tiny::win::write_stdout_utf8("pm_tiny " PM_TINY_VERSION "\n");
        return EXIT_SUCCESS;
    }
    if (g_options.service) {
        const std::wstring service_name = pm_tiny::win::utf8_to_wide(g_options.service_name);
        SERVICE_TABLE_ENTRYW dispatch_table[] = {
            {const_cast<LPWSTR>(service_name.c_str()), service_main},
            {nullptr, nullptr}
        };
        if (!StartServiceCtrlDispatcherW(dispatch_table)) {
            log_error("StartServiceCtrlDispatcher failed with error code " +
                      std::to_string(GetLastError()));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        log_error("Failed to install console control handler.");
    }
    return run_daemon(g_options);
}
