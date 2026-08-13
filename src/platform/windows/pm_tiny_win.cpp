#include "control_server.h"
#include "process_runner.h"
#include "win_config_loader.h"
#include "win_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

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

struct CommandLineOptions {
    std::string config_path;
    std::string service_name = "pm_tiny";
    std::string pipe_name;
    std::string pipe_sddl;
    bool service = false;
};

CommandLineOptions parse_arguments(int argc, char *argv[]) {
    CommandLineOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                options.config_path = argv[++i];
            }
        } else if (arg == "--service") {
            options.service = true;
        } else if (arg == "--service-name" && i + 1 < argc) {
            options.service_name = argv[++i];
        } else if (arg == "--pipe-name" && i + 1 < argc) {
            options.pipe_name = argv[++i];
        } else if (arg == "--pipe-sddl" && i + 1 < argc) {
            options.pipe_sddl = argv[++i];
        }
    }
    if (options.config_path.empty()) {
        options.config_path = "pm_tiny_win.yaml";
    }
    return options;
}

void log_info(const std::string &message) {
    std::cout << "[INFO] " << message << std::endl;
}

void log_error(const std::string &message) {
    std::cerr << "[ERROR] " << message << std::endl;
}

CommandLineOptions g_options;
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

int run_daemon(const CommandLineOptions &options) {
    g_should_stop.store(false);
    if (!options.pipe_name.empty() &&
        _putenv_s("PM_TINY_PIPE_NAME", options.pipe_name.c_str()) != 0) {
        log_error("Failed to set PM_TINY_PIPE_NAME.");
        return EXIT_FAILURE;
    }
    if (!options.pipe_sddl.empty() &&
        _putenv_s("PM_TINY_PIPE_SDDL", options.pipe_sddl.c_str()) != 0) {
        log_error("Failed to set PM_TINY_PIPE_SDDL.");
        return EXIT_FAILURE;
    }

    auto cfg_result = pm_tiny::win::load_program_configs(options.config_path);
    if (!cfg_result.error_message.empty()) {
        log_error(cfg_result.error_message);
    }

    bool has_initial_programs = !cfg_result.programs.empty();
    if (!has_initial_programs) {
        log_info("No preconfigured programs; waiting for CLI commands.");
    } else {
        log_info("Loaded " + std::to_string(cfg_result.programs.size()) + " program entries.");
    }

    std::unordered_map<std::string, pm_tiny::win::ProgramConfig> config_map;
    for (const auto &cfg : cfg_result.programs) {
        config_map[cfg.name] = cfg;
    }

    std::mutex processes_mutex;
    std::vector<pm_tiny::win::ProcessHandle> processes;
    processes.reserve(cfg_result.programs.size());
    pm_tiny::win::RuntimeControlState runtime_state;
    std::string dependency_error;
    if (!pm_tiny::win::rebuild_dependencies(runtime_state, cfg_result.programs, dependency_error)) {
        log_error(dependency_error);
        return EXIT_FAILURE;
    }
    pm_tiny::win::ensure_process_records(processes, cfg_result.programs);

    pm_tiny::win::ControlServer control_server(processes, processes_mutex, config_map, runtime_state,
                                               g_should_stop, options.config_path);
    if (!control_server.start()) {
        log_error("Failed to start control server.");
        return EXIT_FAILURE;
    }
    if (options.service) report_service_status(SERVICE_RUNNING, NO_ERROR, 0);

    if (has_initial_programs) {
        std::lock_guard<std::mutex> lock(processes_mutex);
        const auto failures = pm_tiny::win::schedule_dependency_launch(
            processes, runtime_state, runtime_state.dependencies.request_all());
        for (const auto &failure : failures) log_error("Failed to start program `" + failure);
    }

    bool shutdown_scheduled = false;
    while (true) {
        bool shutdown_complete = false;
        {
            std::lock_guard<std::mutex> lock(processes_mutex);
            const auto now_ms = pm_tiny::win::monotonic_millis();

            if (g_should_stop.load() && !shutdown_scheduled) {
                log_info("Termination requested. Stopping child processes...");
                shutdown_scheduled = true;
                runtime_state.reload_pending = false;
                runtime_state.reload_programs.clear();
                runtime_state.reload_completed.notify_all();
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
                        std::cerr << "[WARN] " << iter->config.name << ": " << error << std::endl;
                    }
                }
            }

            for (auto it = processes.begin(); it != processes.end();) {
                auto &proc = *it;
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

                DWORD root_exit_code = STILL_ACTIVE;
                const bool root_status_ok = GetExitCodeProcess(proc.proc_info.hProcess, &root_exit_code) != FALSE;
                const bool root_active = root_status_ok && root_exit_code == STILL_ACTIVE;
                const bool start_timed_out = root_active && proc.termination_phase == pm_tiny::win::TerminationPhase::none &&
                    proc.config.start_timeout > 0 && !proc.ready &&
                    now_ms - proc.launch_time_ms > static_cast<unsigned long long>(proc.config.start_timeout) * 1000ULL;
                const bool heartbeat_timed_out = root_active && proc.termination_phase == pm_tiny::win::TerminationPhase::none &&
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
                        std::cerr << "[WARN] " << proc.config.name << ": " << terminate_error << std::endl;
                    }
                    if (start_timed_out) runtime_state.dependencies.mark_failed(proc.config.name);
                }

                bool tree_empty = false;
                std::string poll_error;
                const auto phase_before_poll = proc.termination_phase;
                if (!pm_tiny::win::poll_program_termination(proc, now_ms, tree_empty, poll_error)) {
                    log_error("Failed to poll `" + proc.config.name + "` generation " +
                              std::to_string(proc.generation) + ": " + poll_error);
                    ++it;
                    continue;
                }
                if (phase_before_poll == pm_tiny::win::TerminationPhase::graceful_requested &&
                    proc.termination_phase == pm_tiny::win::TerminationPhase::force_kill_requested) {
                    std::cerr << "[WARN] Program `" << proc.config.name << "` generation "
                              << proc.generation << " exceeded kill_timeout; forced Job Object termination."
                              << std::endl;
                }
                if (!root_active && !tree_empty && proc.termination_phase == pm_tiny::win::TerminationPhase::none) {
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
                        std::vector<pm_tiny::win::ProgramConfig> remaining;
                        remaining.reserve(processes.size());
                        for (const auto &record : processes) remaining.push_back(record.config);
                        std::string rebuild_error;
                        if (!pm_tiny::win::rebuild_dependencies(runtime_state, remaining, rebuild_error))
                            log_error(rebuild_error);
                    } else {
                        ++it;
                    }
                }
            }

            const bool all_processes_stopped = std::none_of(processes.begin(), processes.end(),
                [](const auto &proc) { return proc.has_process || proc.termination_phase != pm_tiny::win::TerminationPhase::none; });
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
                    const auto failures = pm_tiny::win::schedule_dependency_launch(
                        processes, runtime_state, runtime_state.dependencies.request_all());
                    for (const auto &failure : failures) log_error("Failed to reload program `" + failure);
                }
                runtime_state.reload_programs.clear();
                runtime_state.reload_pending = false;
                runtime_state.reload_completed.notify_all();
            }

            shutdown_complete = shutdown_scheduled && all_processes_stopped;
        }
        if (shutdown_complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

int main(int argc, char *argv[]) {
    g_options = parse_arguments(argc, argv);
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
