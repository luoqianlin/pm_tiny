#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log_writer.h"
#include "win_config_loader.h"
#include "restart_policy.h"
#include "dependency_graph.h"

namespace pm_tiny {
namespace win {

enum class TerminationPhase {
    none,
    graceful_requested,
    force_kill_requested
};

enum class CompletionAction {
    automatic,
    remove,
    restart,
    delete_config
};

struct ProcessHandle {
    ProgramConfig config;
    PROCESS_INFORMATION proc_info{};
    bool has_process = false;
    HANDLE job = nullptr;

    HANDLE pipe_read = nullptr;
    std::unique_ptr<LogWriter> log_writer;
    std::thread log_thread;
    std::shared_ptr<std::atomic_bool> log_thread_running = std::make_shared<std::atomic_bool>(false);
    bool disable_restart = false;
    bool ready = false;
    unsigned long long launch_time_ms = 0;
    unsigned long long last_tick_ms = 0;
    unsigned long long generation = 0;
    std::int32_t restart_count = 0;
    TerminationPhase termination_phase = TerminationPhase::none;
    CompletionAction completion_action = CompletionAction::automatic;
    unsigned long long termination_deadline_ms = 0;
    unsigned long termination_exit_code = 0;
    bool restart_pending = false;
    unsigned long long restart_due_ms = 0;
    restart_policy_state restart_state;

    ProcessHandle() = default;
    ~ProcessHandle();

    ProcessHandle(const ProcessHandle &) = delete;
    ProcessHandle &operator=(const ProcessHandle &) = delete;

    ProcessHandle(ProcessHandle &&other) noexcept;
    ProcessHandle &operator=(ProcessHandle &&other) noexcept;

    void reset();
};

struct RuntimeControlState {
    dependency_graph graph;
    dependency_runtime dependencies;
    bool reload_pending = false;
    std::vector<ProgramConfig> reload_programs;
    std::condition_variable reload_completed;
};

bool rebuild_dependencies(RuntimeControlState &state,
                          const std::vector<ProgramConfig> &configs,
                          std::string &error_message);
void ensure_process_records(std::vector<ProcessHandle> &processes,
                            const std::vector<ProgramConfig> &configs);
std::vector<std::string> schedule_dependency_launch(std::vector<ProcessHandle> &processes,
                                                    RuntimeControlState &state,
                                                    const std::vector<std::string> &names);

bool launch_program(ProcessHandle &handle, std::string &error_message);
bool request_program_termination(ProcessHandle &handle,
                                 CompletionAction completion_action,
                                 unsigned long exit_code,
                                 std::string &error_message);
bool poll_program_termination(ProcessHandle &handle,
                              unsigned long long now_ms,
                              bool &tree_empty,
                              std::string &error_message);
bool is_process_tree_empty(const ProcessHandle &handle, bool &tree_empty, std::string &error_message);
bool force_terminate_program(ProcessHandle &handle, unsigned long exit_code, std::string &error_message);

struct WaitResult {
    bool has_event = false;
    size_t index = 0;
};

WaitResult wait_for_handles(const std::vector<HANDLE> &handles,
                            unsigned long timeout_ms,
                            std::string &error_message);

void terminate_all(std::vector<ProcessHandle> &processes, unsigned long wait_timeout_ms);
unsigned long long monotonic_millis();

} // namespace win
} // namespace pm_tiny
