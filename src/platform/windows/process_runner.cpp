#include "process_runner.h"

#include "win_utils.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace pm_tiny {
namespace win {

unsigned long long monotonic_millis() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

std::atomic_ullong next_generation{1};

std::vector<wchar_t> to_mutable_command_line(const std::string &command) {
    auto wide = utf8_to_wide(command);
    std::vector<wchar_t> buffer(wide.begin(), wide.end());
    buffer.push_back(L'\0');
    return buffer;
}

void close_handle_safe(HANDLE &handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void start_logging_thread(ProcessHandle &handle) {
    if (handle.pipe_read == nullptr || handle.pipe_read == INVALID_HANDLE_VALUE) {
        return;
    }
    auto running = handle.log_thread_running;
    auto pipe_read = handle.pipe_read;
    auto log_writer = handle.log_writer.get();
    running->store(true);
    handle.log_thread = std::thread([running, pipe_read, log_writer]() {
        const DWORD buffer_size = 4096;
        std::vector<char> buffer(buffer_size);
        while (running->load()) {
            DWORD bytes_read = 0;
            BOOL ok = ReadFile(pipe_read, buffer.data(), buffer_size, &bytes_read, nullptr);
            if (!ok || bytes_read == 0) {
                break;
            }
            try {
                if (log_writer != nullptr) {
                    log_writer->append(buffer.data(), static_cast<std::size_t>(bytes_read));
                }
            } catch (const std::exception &ex) {
                std::cerr << "[ERROR] log write failed: " << ex.what() << std::endl;
            }
        }
        if (log_writer != nullptr) {
            try {
                log_writer->flush();
            } catch (...) {
            }
        }
        running->store(false);
    });
}

} // namespace

ProcessHandle::~ProcessHandle() {
    reset();
}

ProcessHandle::ProcessHandle(ProcessHandle &&other) noexcept {
    *this = std::move(other);
}

ProcessHandle &ProcessHandle::operator=(ProcessHandle &&other) noexcept {
    if (this != &other) {
        reset();
        config = std::move(other.config);
        proc_info = other.proc_info;
        has_process = other.has_process;
        job = other.job;
        pipe_read = other.pipe_read;
        log_writer = std::move(other.log_writer);
        log_thread = std::move(other.log_thread);
        log_thread_running = std::move(other.log_thread_running);
        disable_restart = other.disable_restart;
        ready = other.ready;
        launch_time_ms = other.launch_time_ms;
        last_tick_ms = other.last_tick_ms;
        generation = other.generation;
        restart_count = other.restart_count;
        termination_phase = other.termination_phase;
        completion_action = other.completion_action;
        termination_deadline_ms = other.termination_deadline_ms;
        termination_exit_code = other.termination_exit_code;
        restart_pending = other.restart_pending;
        restart_due_ms = other.restart_due_ms;
        restart_state = std::move(other.restart_state);

        other.proc_info = PROCESS_INFORMATION{};
        other.has_process = false;
        other.job = nullptr;
        other.pipe_read = nullptr;
        other.log_thread_running = std::make_shared<std::atomic_bool>(false);
        other.disable_restart = false;
        other.ready = false;
        other.launch_time_ms = 0;
        other.last_tick_ms = 0;
        other.generation = 0;
        other.restart_count = 0;
        other.termination_phase = TerminationPhase::none;
        other.completion_action = CompletionAction::automatic;
        other.termination_deadline_ms = 0;
        other.termination_exit_code = 0;
        other.restart_pending = false;
        other.restart_due_ms = 0;
        other.restart_state.reset();
    }
    return *this;
}

void ProcessHandle::reset() {
    if (log_thread.joinable()) {
        log_thread_running->store(false);
        close_handle_safe(pipe_read);
        log_thread.join();
    }
    log_writer.reset();
    close_handle_safe(pipe_read);
    close_handle_safe(job);
    if (has_process) {
        close_handle_safe(proc_info.hThread);
        close_handle_safe(proc_info.hProcess);
        has_process = false;
    }
    proc_info = PROCESS_INFORMATION{};
    ready = false;
    launch_time_ms = 0;
    last_tick_ms = 0;
    termination_phase = TerminationPhase::none;
    completion_action = CompletionAction::automatic;
    termination_deadline_ms = 0;
    termination_exit_code = 0;
    if (!log_thread_running) log_thread_running = std::make_shared<std::atomic_bool>(false);
}

bool launch_program(ProcessHandle &handle, std::string &error_message) {
    const bool has_launched_before = handle.generation != 0;
    handle.reset();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE pipe_read = nullptr;
    HANDLE pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        error_message = "CreatePipe failed with error code " + std::to_string(GetLastError());
        return false;
    }
    if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
        close_handle_safe(pipe_read);
        close_handle_safe(pipe_write);
        error_message = "SetHandleInformation failed with error code " + std::to_string(GetLastError());
        return false;
    }

    STARTUPINFOW startup_info;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.hStdOutput = pipe_write;
    startup_info.hStdError = pipe_write;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        close_handle_safe(pipe_read);
        close_handle_safe(pipe_write);
        error_message = "CreateJobObjectW failed with error code " + std::to_string(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
        const auto last_error = GetLastError();
        close_handle_safe(job);
        close_handle_safe(pipe_read);
        close_handle_safe(pipe_write);
        error_message = "SetInformationJobObject failed with error code " + std::to_string(last_error);
        return false;
    }

    std::vector<wchar_t> cmd_line;
    try {
        cmd_line = to_mutable_command_line(handle.config.command);
    } catch (const std::exception &ex) {
        close_handle_safe(pipe_read);
        close_handle_safe(pipe_write);
        close_handle_safe(job);
        error_message = std::string("Failed to convert command to UTF-16: ") + ex.what();
        return false;
    }

    std::wstring cwd_w;
    LPCWSTR cwd_ptr = nullptr;
    if (!handle.config.cwd.empty()) {
        try {
            cwd_w = utf8_to_wide(handle.config.cwd);
            cwd_ptr = cwd_w.c_str();
        } catch (const std::exception &ex) {
            close_handle_safe(pipe_read);
            close_handle_safe(pipe_write);
            close_handle_safe(job);
            error_message = std::string("Failed to convert working directory: ") + ex.what();
            return false;
        }
    }

    std::vector<std::string> environment = handle.config.env_vars;
    environment.push_back("PM_TINY_APP_NAME=" + handle.config.name);
    environment.push_back("PM_TINY_PIPE_NAME=" + control_pipe_name());
    std::vector<wchar_t> env_block;
    LPWSTR environment_ptr = nullptr;
    if (!environment.empty()) {
        try {
            env_block = build_environment_block(environment);
            if (!env_block.empty()) {
                environment_ptr = env_block.data();
            }
        } catch (const std::exception &ex) {
            std::cerr << "[ERROR] Failed to build environment block: " << ex.what() << std::endl;
        }
    }
    BOOL success = CreateProcessW(
        nullptr,
        cmd_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
        environment_ptr,
        cwd_ptr,
        &startup_info,
        &process_info);

    close_handle_safe(pipe_write);

    if (!success) {
        const auto last_error = GetLastError();
        close_handle_safe(pipe_read);
        close_handle_safe(job);
        error_message = "CreateProcessW failed with error code " + std::to_string(last_error);
        return false;
    }

    if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        const auto last_error = GetLastError();
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 3000);
        close_handle_safe(process_info.hThread);
        close_handle_safe(process_info.hProcess);
        close_handle_safe(pipe_read);
        close_handle_safe(job);
        error_message = "AssignProcessToJobObject failed with error code " + std::to_string(last_error);
        return false;
    }
    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
        const auto last_error = GetLastError();
        TerminateJobObject(job, 1);
        WaitForSingleObject(process_info.hProcess, 3000);
        close_handle_safe(process_info.hThread);
        close_handle_safe(process_info.hProcess);
        close_handle_safe(pipe_read);
        close_handle_safe(job);
        error_message = "ResumeThread failed with error code " + std::to_string(last_error);
        return false;
    }

    handle.proc_info = process_info;
    handle.has_process = true;
    handle.job = job;
    handle.ready = false;
    handle.generation = next_generation.fetch_add(1);
    if (has_launched_before) ++handle.restart_count;
    handle.launch_time_ms = monotonic_millis();
    handle.last_tick_ms = handle.launch_time_ms;
    handle.pipe_read = pipe_read;

    std::string log_file_name = handle.config.log_file_name;
    if (log_file_name.empty()) {
        log_file_name = handle.config.name.empty() ? "pm_tiny.log" : handle.config.name + ".log";
    }
    std::size_t max_size_bytes = static_cast<std::size_t>(handle.config.log_max_size_kb) * 1024ULL;
    if (max_size_bytes == 0) {
        max_size_bytes = 4 * 1024 * 1024ULL;
    }
    int log_files = handle.config.log_file_count > 0 ? handle.config.log_file_count : 3;
    try {
        handle.log_writer = std::make_unique<LogWriter>(handle.config.log_dir,
                                                        log_file_name,
                                                        max_size_bytes,
                                                        log_files);
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] Failed to create log writer for " << handle.config.name
                  << ": " << ex.what() << std::endl;
    }
    handle.disable_restart = false;
    handle.termination_phase = TerminationPhase::none;
    handle.completion_action = CompletionAction::automatic;
    handle.termination_deadline_ms = 0;
    handle.termination_exit_code = 0;
    handle.restart_pending = false;
    handle.restart_due_ms = 0;
    start_logging_thread(handle);

    return true;
}

bool rebuild_dependencies(RuntimeControlState &state,
                          const std::vector<ProgramConfig> &configs,
                          std::string &error_message) {
    std::vector<dependency_node_config> nodes;
    nodes.reserve(configs.size());
    for (const auto &config : configs) nodes.push_back({config.name, config.depends_on});
    dependency_error error;
    dependency_graph graph;
    if (!dependency_graph::build(nodes, graph, error)) {
        error_message = error.message;
        return false;
    }
    state.graph = std::move(graph);
    state.dependencies.reset(state.graph);
    error_message.clear();
    return true;
}

void ensure_process_records(std::vector<ProcessHandle> &processes,
                            const std::vector<ProgramConfig> &configs) {
    for (const auto &config : configs) {
        const auto found = std::find_if(processes.begin(), processes.end(), [&](const ProcessHandle &process) {
            return process.config.name == config.name;
        });
        if (found == processes.end()) {
            processes.emplace_back();
            processes.back().config = config;
        } else {
            found->config = config;
        }
    }
}

std::vector<std::string> schedule_dependency_launch(std::vector<ProcessHandle> &processes,
                                                    RuntimeControlState &state,
                                                    const std::vector<std::string> &initial_names) {
    std::vector<std::string> failures;
    std::vector<std::string> pending = initial_names;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto &name = pending[i];
        auto found = std::find_if(processes.begin(), processes.end(), [&](const ProcessHandle &process) {
            return process.config.name == name;
        });
        if (found == processes.end()) continue;
        if (found->has_process) continue;
        found->disable_restart = false;
        found->restart_pending = false;
        found->restart_due_ms = 0;
        found->restart_state.reset();
        std::string error_message;
        if (!launch_program(*found, error_message)) {
            failures.push_back(name + ": " + error_message);
            state.dependencies.mark_failed(name);
            continue;
        }
        state.dependencies.mark_starting(name);
        if (found->config.start_timeout == 0) {
            found->ready = true;
            const auto unlocked = state.dependencies.mark_ready(name);
            pending.insert(pending.end(), unlocked.begin(), unlocked.end());
        }
    }
    return failures;
}

bool force_terminate_program(ProcessHandle &handle, unsigned long exit_code, std::string &error_message) {
    error_message.clear();
    if (!handle.has_process || handle.proc_info.hProcess == nullptr) return true;
    if (handle.job != nullptr) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(handle.job,
                                       JobObjectBasicAccountingInformation,
                                       &accounting,
                                       sizeof(accounting),
                                       nullptr)) {
            error_message = "QueryInformationJobObject failed with error code " +
                            std::to_string(GetLastError());
            return false;
        }
        if (accounting.ActiveProcesses == 0) return true;
        if (!TerminateJobObject(handle.job, exit_code)) {
            error_message = "Failed to terminate process tree with error code " + std::to_string(GetLastError());
            return false;
        }
        return true;
    }
    DWORD current_exit_code = 0;
    if (!GetExitCodeProcess(handle.proc_info.hProcess, &current_exit_code)) {
        error_message = "GetExitCodeProcess failed with error code " + std::to_string(GetLastError());
        return false;
    }
    if (current_exit_code != STILL_ACTIVE) return true;
    if (!TerminateProcess(handle.proc_info.hProcess, exit_code)) {
        error_message = "Failed to terminate process tree with error code " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool is_process_tree_empty(const ProcessHandle &handle, bool &tree_empty, std::string &error_message) {
    error_message.clear();
    tree_empty = true;
    if (!handle.has_process) return true;
    if (handle.job != nullptr) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(handle.job,
                                       JobObjectBasicAccountingInformation,
                                       &accounting,
                                       sizeof(accounting),
                                       nullptr)) {
            error_message = "QueryInformationJobObject failed with error code " +
                            std::to_string(GetLastError());
            tree_empty = false;
            return false;
        }
        tree_empty = accounting.ActiveProcesses == 0;
        return true;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(handle.proc_info.hProcess, &exit_code)) {
        error_message = "GetExitCodeProcess failed with error code " + std::to_string(GetLastError());
        tree_empty = false;
        return false;
    }
    tree_empty = exit_code != STILL_ACTIVE;
    return true;
}

bool request_program_termination(ProcessHandle &handle,
                                 CompletionAction completion_action,
                                 unsigned long exit_code,
                                 std::string &error_message) {
    error_message.clear();
    if (!handle.has_process || handle.proc_info.hProcess == nullptr) return true;
    handle.completion_action = completion_action;
    handle.termination_exit_code = exit_code;
    if (handle.termination_phase != TerminationPhase::none) return true;

    const auto timeout_ms = handle.config.kill_timeout_s > 0
                            ? static_cast<unsigned long long>(handle.config.kill_timeout_s) * 1000ULL
                            : 0ULL;
    handle.termination_deadline_ms = monotonic_millis() + timeout_ms;
    if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, handle.proc_info.dwProcessId)) {
        handle.termination_phase = TerminationPhase::graceful_requested;
        return true;
    }

    const auto signal_error = GetLastError();
    handle.termination_phase = TerminationPhase::force_kill_requested;
    std::string force_error;
    if (!force_terminate_program(handle, exit_code, force_error)) {
        error_message = "GenerateConsoleCtrlEvent failed with error code " + std::to_string(signal_error) +
                        "; " + force_error;
        return false;
    }
    error_message = "graceful termination unavailable (error code " + std::to_string(signal_error) +
                    "); forced process tree termination";
    return true;
}

bool poll_program_termination(ProcessHandle &handle,
                              unsigned long long now_ms,
                              bool &tree_empty,
                              std::string &error_message) {
    if (!is_process_tree_empty(handle, tree_empty, error_message)) return false;
    if (tree_empty || handle.termination_phase != TerminationPhase::graceful_requested ||
        now_ms < handle.termination_deadline_ms) {
        return true;
    }
    handle.termination_phase = TerminationPhase::force_kill_requested;
    return force_terminate_program(handle, handle.termination_exit_code, error_message);
}

WaitResult wait_for_handles(const std::vector<HANDLE> &handles,
                            unsigned long timeout_ms,
                            std::string &error_message) {
    WaitResult result;
    if (handles.empty()) {
        return result;
    }
    if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
        error_message = "Too many processes; Windows WaitForMultipleObjects limit exceeded.";
        return result;
    }
    DWORD wait_rc = WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                           handles.data(),
                                           FALSE,
                                           timeout_ms);
    if (wait_rc == WAIT_FAILED) {
        auto last_error = GetLastError();
        error_message = "WaitForMultipleObjects failed with error code " + std::to_string(last_error);
        return result;
    }
    if (wait_rc == WAIT_TIMEOUT) {
        return result;
    }
    DWORD base = WAIT_OBJECT_0;
    if (wait_rc >= base && wait_rc < base + handles.size()) {
        result.has_event = true;
        result.index = static_cast<size_t>(wait_rc - base);
        return result;
    }
    DWORD abandon_base = WAIT_ABANDONED_0;
    if (wait_rc >= abandon_base && wait_rc < abandon_base + handles.size()) {
        result.has_event = true;
        result.index = static_cast<size_t>(wait_rc - abandon_base);
        return result;
    }
    error_message = "Unexpected WaitForMultipleObjects return value: " + std::to_string(wait_rc);
    return result;
}

void terminate_all(std::vector<ProcessHandle> &processes, unsigned long wait_timeout_ms) {
    for (auto iter = processes.rbegin(); iter != processes.rend(); ++iter) {
        auto &proc = *iter;
        if (!proc.has_process) {
            proc.reset();
            continue;
        }
        unsigned long wait_ms = wait_timeout_ms;
        if (proc.config.kill_timeout_s > 0) {
            wait_ms = static_cast<unsigned long>(proc.config.kill_timeout_s) * 1000UL;
        }
        std::string terminate_error;
        if (!force_terminate_program(proc, 0, terminate_error)) {
            std::cerr << "[ERROR] " << terminate_error << std::endl;
        }
        WaitForSingleObject(proc.proc_info.hProcess, wait_ms);
        proc.reset();
    }
    processes.clear();
}

} // namespace win
} // namespace pm_tiny
