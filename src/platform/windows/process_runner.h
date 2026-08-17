#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "program_log.h"
#include "rotating_log_writer.h"
#include "win_config_loader.h"
#include "restart_policy.h"
#include "dependency_graph.h"
#include "termination_job.h"

namespace pm_tiny {
namespace win {

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

    HANDLE pipe_read[2]{nullptr, nullptr};
    std::unique_ptr<rotating_log_writer> log_writers[2];
    bounded_log_tail log_tail;
    log_sink_health log_health;
    std::vector<std::string> log_paths;
    bool log_pipe_eof[2]{false, false};
    unsigned long long watched_log_generation[2]{0, 0};
    bool disable_restart = false;
    bool ready = false;
    unsigned long long launch_time_ms = 0;
    unsigned long long last_tick_ms = 0;
    unsigned long long generation = 0;
    unsigned long long watched_generation = 0;
    bool root_exit_observed = false;
    unsigned long root_exit_code = STILL_ACTIVE;
    bool has_last_exit = false;
    unsigned long last_exit_code = 0;
    std::int64_t last_exit_time_unix_ms = 0;
    unsigned long last_pid = 0;
    std::int32_t restart_count = 0;
    termination_job termination;
    CompletionAction completion_action = CompletionAction::automatic;
    unsigned long termination_exit_code = 0;
    bool restart_pending = false;
    unsigned long long restart_due_ms = 0;
    restart_policy_state restart_state;
    std::string config_source = "file";

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
};

bool rebuild_dependencies(RuntimeControlState &state,
                          const std::vector<ProgramConfig> &configs,
                          std::string &error_message);
bool build_dependencies(const std::vector<ProgramConfig> &configs,
                        dependency_graph &graph,
                        std::string &error_message);
void replace_dependencies(RuntimeControlState &state, dependency_graph graph);
void ensure_process_records(std::vector<ProcessHandle> &processes,
                            const std::vector<ProgramConfig> &configs);
std::vector<std::string> schedule_dependency_launch(std::vector<ProcessHandle> &processes,
                                                    RuntimeControlState &state,
                                                    const std::vector<std::string> &names);

bool launch_program(ProcessHandle &handle, std::string &error_message);
void poll_program_log(ProcessHandle &handle);
void append_program_log(ProcessHandle &handle, int stream_index, const char *data, std::size_t size);
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

void terminate_all(std::vector<ProcessHandle> &processes, unsigned long wait_timeout_ms);
unsigned long long monotonic_millis();

} // namespace win
} // namespace pm_tiny
