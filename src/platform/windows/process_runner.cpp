#include "process_runner.h"
#include "core/daemon_config.h"
#include "core/pm_tiny.h"
#include "daemon_log.h"

#include "win_utils.h"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>
#include <unordered_map>

namespace pm_tiny {
namespace win {

unsigned long long monotonic_millis() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {

std::atomic_ullong next_generation{1};
std::atomic_ullong next_log_pipe{1};

std::string quote_windows_argument(const std::string &argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return argument;
    std::string result = "\"";
    std::size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == '\\') { ++slashes; continue; }
        if (ch == '"') result.append(slashes * 2 + 1, '\\');
        else result.append(slashes, '\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, '\\');
    return result + "\"";
}

std::vector<wchar_t> to_mutable_command_line(const ProgramConfig &config) {
    std::string command = quote_windows_argument(config.executable);
    for (const auto &arg : config.args) command += " " + quote_windows_argument(arg);
    auto wide = utf8_to_wide(command);
    std::vector<wchar_t> buffer(wide.begin(), wide.end());
    buffer.push_back(L'\0');
    return buffer;
}

std::string environment_key(const std::string &entry) {
    const auto separator = entry.find('=');
    return separator == std::string::npos ? std::string() : entry.substr(0, separator);
}

std::vector<std::string> effective_environment(const ProgramConfig &config) {
    std::vector<std::string> result;
    std::unordered_map<std::string, std::size_t> indices;
    const auto apply = [&](const std::string &entry) {
        auto key = environment_key(entry);
        if (key.empty()) throw std::runtime_error("invalid environment entry: " + entry);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        const auto found = indices.find(key);
        if (found == indices.end()) {
            indices[key] = result.size();
            result.push_back(entry);
        } else {
            result[found->second] = entry;
        }
    };
    if (config.envs.empty()) {
        auto environment = GetEnvironmentStringsW();
        if (environment == nullptr) throw std::runtime_error("GetEnvironmentStringsW failed");
        try {
            for (const wchar_t *item = environment; *item != L'\0'; item += wcslen(item) + 1) {
                const auto entry = wide_to_utf8(item);
                if (!entry.empty() && entry[0] != '=' && entry.rfind("PM_TINY_", 0) != 0) apply(entry);
            }
        } catch (...) {
            FreeEnvironmentStringsW(environment);
            throw;
        }
        FreeEnvironmentStringsW(environment);
    } else {
        for (const auto &entry : config.envs) apply(entry);
    }
    for (const auto &entry : config.env_vars) apply(entry);
    return result;
}

std::string environment_value(const std::vector<std::string> &environment, const char *wanted) {
    for (const auto &entry : environment) {
        const auto key = environment_key(entry);
        if (_stricmp(key.c_str(), wanted) == 0) return entry.substr(key.size() + 1);
    }
    return {};
}

void close_handle_safe(HANDLE &handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

bool create_overlapped_log_pipe(HANDLE &pipe_read, HANDLE &pipe_write,
                                std::string &error_message) {
    const auto pipe_name = L"\\\\.\\pipe\\pm_tiny_log_" + std::to_wstring(GetCurrentProcessId()) +
                           L"_" + std::to_wstring(next_log_pipe.fetch_add(1));
    pipe_read = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe_read == INVALID_HANDLE_VALUE) {
        pipe_read = nullptr;
        error_message = "CreateNamedPipeW failed with error code " + std::to_string(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    pipe_write = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe_write == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        pipe_write = nullptr;
        close_handle_safe(pipe_read);
        error_message = "CreateFileW log pipe failed with error code " + std::to_string(error);
        return false;
    }
    return true;
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
        for (int i = 0; i < 2; ++i) {
            pipe_read[i] = other.pipe_read[i];
            log_writers[i] = std::move(other.log_writers[i]);
            log_pipe_eof[i] = other.log_pipe_eof[i];
            watched_log_generation[i] = other.watched_log_generation[i];
        }
        log_tail = std::move(other.log_tail);
        log_health = std::move(other.log_health);
        log_paths = std::move(other.log_paths);
        disable_restart = other.disable_restart;
        ready = other.ready;
        launch_time_ms = other.launch_time_ms;
        last_tick_ms = other.last_tick_ms;
        generation = other.generation;
        watched_generation = other.watched_generation;
        root_exit_observed = other.root_exit_observed;
        root_exit_code = other.root_exit_code;
        has_last_exit = other.has_last_exit;
        last_exit_code = other.last_exit_code;
        restart_count = other.restart_count;
        termination = other.termination;
        completion_action = other.completion_action;
        termination_exit_code = other.termination_exit_code;
        restart_pending = other.restart_pending;
        restart_due_ms = other.restart_due_ms;
        restart_state = std::move(other.restart_state);
        config_source = std::move(other.config_source);

        other.proc_info = PROCESS_INFORMATION{};
        other.has_process = false;
        other.job = nullptr;
        for (int i = 0; i < 2; ++i) {
            other.pipe_read[i] = nullptr;
            other.log_pipe_eof[i] = false;
            other.watched_log_generation[i] = 0;
        }
        other.disable_restart = false;
        other.ready = false;
        other.launch_time_ms = 0;
        other.last_tick_ms = 0;
        other.generation = 0;
        other.watched_generation = 0;
        other.root_exit_observed = false;
        other.root_exit_code = STILL_ACTIVE;
        other.has_last_exit = false;
        other.last_exit_code = 0;
        other.restart_count = 0;
        other.termination = termination_job{};
        other.completion_action = CompletionAction::automatic;
        other.termination_exit_code = 0;
        other.restart_pending = false;
        other.restart_due_ms = 0;
        other.restart_state.reset();
        other.config_source = "file";
    }
    return *this;
}

void ProcessHandle::reset() {
    poll_program_log(*this);
    for (int i = 0; i < 2; ++i) {
        if (log_writers[i]) { std::string ignored; log_writers[i]->flush(ignored); }
        log_writers[i].reset();
        close_handle_safe(pipe_read[i]);
    }
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
    termination = termination_job{};
    completion_action = CompletionAction::automatic;
    termination_exit_code = 0;
    log_pipe_eof[0] = log_pipe_eof[1] = false;
    watched_log_generation[0] = watched_log_generation[1] = 0;
    watched_generation = 0;
    root_exit_observed = false;
    root_exit_code = STILL_ACTIVE;
}

bool launch_program(ProcessHandle &handle, std::string &error_message) {
    const bool has_launched_before = handle.generation != 0;
    handle.reset();

    HANDLE pipe_read[2]{nullptr, nullptr};
    HANDLE pipe_write[2]{nullptr, nullptr};
    const auto close_pipes = [&]() {
        for (int i = 0; i < 2; ++i) { close_handle_safe(pipe_read[i]); close_handle_safe(pipe_write[i]); }
    };
    const int stream_count = handle.config.log_mode == log_mode_t::split ? 2 : 1;
    for (int i = 0; i < stream_count; ++i) {
        if (!create_overlapped_log_pipe(pipe_read[i], pipe_write[i], error_message)) {
            for (int j = 0; j < 2; ++j) { close_handle_safe(pipe_read[j]); close_handle_safe(pipe_write[j]); }
            return false;
        }
    }

    STARTUPINFOW startup_info;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.hStdOutput = pipe_write[0];
    startup_info.hStdError = stream_count == 2 ? pipe_write[1] : pipe_write[0];
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        close_pipes();
        error_message = "CreateJobObjectW failed with error code " + std::to_string(GetLastError());
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
        const auto last_error = GetLastError();
        close_handle_safe(job);
        close_pipes();
        error_message = "SetInformationJobObject failed with error code " + std::to_string(last_error);
        return false;
    }

    std::vector<wchar_t> cmd_line;
    try {
        cmd_line = to_mutable_command_line(handle.config);
    } catch (const std::exception &ex) {
        close_pipes();
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
            close_pipes();
            close_handle_safe(job);
            error_message = std::string("Failed to convert working directory: ") + ex.what();
            return false;
        }
    }

    std::vector<std::string> environment;
    try { environment = effective_environment(handle.config); }
    catch (const std::exception &ex) {
        close_pipes(); close_handle_safe(job);
        error_message = std::string("environment: ") + ex.what();
        return false;
    }
    environment.push_back("PM_TINY_APP_NAME=" + handle.config.name);
    environment.push_back(std::string("PM_TINY_HOME=") + daemon_environment(PM_TINY_HOME));
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
            close_pipes(); close_handle_safe(job);
            error_message = std::string("environment: ") + ex.what();
            return false;
        }
    }
    std::wstring executable_w;
    try { executable_w = utf8_to_wide(handle.config.executable); }
    catch (const std::exception &ex) {
        close_pipes(); close_handle_safe(job);
        error_message = std::string("Failed to convert executable to UTF-16: ") + ex.what();
        return false;
    }
    if (executable_w.find(L'\\') == std::wstring::npos && executable_w.find(L'/') == std::wstring::npos) {
        std::vector<wchar_t> resolved(32768);
        std::wstring search_path;
        try { search_path = utf8_to_wide(environment_value(environment, "PATH")); }
        catch (const std::exception &ex) {
            close_pipes(); close_handle_safe(job);
            error_message = std::string("resolve: invalid PATH: ") + ex.what();
            return false;
        }
        const DWORD length = SearchPathW(search_path.empty() ? L"" : search_path.c_str(), executable_w.c_str(), nullptr,
                                         static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
        if (length == 0 || length >= resolved.size()) {
            close_pipes(); close_handle_safe(job);
            error_message = "resolve: executable not found: " + handle.config.executable;
            return false;
        }
        executable_w.assign(resolved.data(), length);
    }
    BOOL success = CreateProcessW(
        executable_w.c_str(),
        cmd_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
        environment_ptr,
        cwd_ptr,
        &startup_info,
        &process_info);

    for (auto &pipe : pipe_write) close_handle_safe(pipe);

    if (!success) {
        const auto last_error = GetLastError();
        for (auto &pipe : pipe_read) close_handle_safe(pipe);
        close_handle_safe(job);
        error_message = "spawn: CreateProcessW failed with error code " + std::to_string(last_error);
        return false;
    }

    if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        const auto last_error = GetLastError();
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 3000);
        close_handle_safe(process_info.hThread);
        close_handle_safe(process_info.hProcess);
        for (auto &pipe : pipe_read) close_handle_safe(pipe);
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
        for (auto &pipe : pipe_read) close_handle_safe(pipe);
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
    for (int i = 0; i < stream_count; ++i) handle.pipe_read[i] = pipe_read[i];

    const char *default_log_dir = std::getenv("PM_TINY_APP_LOG_DIR");
    handle.log_paths = derive_log_paths(handle.config.log_dir.empty()
                                            ? (default_log_dir ? default_log_dir : "logs")
                                            : handle.config.log_dir,
                                        handle.config.name, handle.config.log_mode,
                                        handle.config.log_file_name);
    handle.log_health.reset();
    for (std::size_t i = 0; i < handle.log_paths.size(); ++i) {
        handle.log_writers[i].reset(new rotating_log_writer(
            handle.log_paths[i], static_cast<std::size_t>(handle.config.log_max_size_kb) * 1024U,
            handle.config.log_archive_count));
        std::string log_error;
        if (!handle.log_writers[i]->open(log_error)) {
            handle.log_writers[i].reset();
            handle.log_health.record_failure(monotonic_millis(), 0, log_error);
            daemon_log_message(daemon_log_level_t::error, "Program `" + handle.config.name + "` log degraded: " + log_error);
        }
    }
    handle.disable_restart = false;
    handle.termination = termination_job{};
    handle.completion_action = CompletionAction::automatic;
    handle.termination_exit_code = 0;
    handle.restart_pending = false;
    handle.restart_due_ms = 0;
    handle.log_tail.clear();
    handle.log_pipe_eof[0] = handle.log_pipe_eof[1] = false;
    handle.watched_log_generation[0] = handle.watched_log_generation[1] = 0;

    return true;
}

void poll_program_log(ProcessHandle &handle) {
    for (int stream = 0; stream < 2; ++stream) for (;;) {
        if (handle.pipe_read[stream] == nullptr || handle.pipe_read[stream] == INVALID_HANDLE_VALUE ||
            handle.log_pipe_eof[stream]) break;
        DWORD available = 0;
        if (!PeekNamedPipe(handle.pipe_read[stream], nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) handle.log_pipe_eof[stream] = true;
            break;
        }
        if (available == 0) break;
        std::vector<char> buffer(std::min<DWORD>(available, 16 * 1024));
        DWORD bytes_read = 0;
        if (!ReadFile(handle.pipe_read[stream], buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) ||
            bytes_read == 0) {
            handle.log_pipe_eof[stream] = true;
            break;
        }
        append_program_log(handle, stream, buffer.data(), bytes_read);
    }
}

void append_program_log(ProcessHandle &handle, int stream_index, const char *data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    handle.log_tail.append(data, size);
    const int writer_index = handle.config.log_mode == log_mode_t::combined ? 0 : stream_index;
    const auto now = monotonic_millis();
    if (!handle.log_writers[writer_index] && (!handle.log_health.degraded || handle.log_health.retry_ready(now))) {
        handle.log_writers[writer_index].reset(new rotating_log_writer(
            handle.log_paths[writer_index], static_cast<std::size_t>(handle.config.log_max_size_kb) * 1024U,
            handle.config.log_archive_count));
        std::string open_error;
        if (!handle.log_writers[writer_index]->open(open_error)) {
            handle.log_writers[writer_index].reset();
            handle.log_health.record_failure(now, size, open_error);
            return;
        }
        const int writer_count = handle.config.log_mode == log_mode_t::combined ? 1 : 2;
        for (int i = 0; i < writer_count; ++i) {
            if (handle.log_writers[i]) continue;
            handle.log_writers[i].reset(new rotating_log_writer(
                handle.log_paths[i], static_cast<std::size_t>(handle.config.log_max_size_kb) * 1024U,
                handle.config.log_archive_count));
            std::string sibling_error;
            if (!handle.log_writers[i]->open(sibling_error)) {
                handle.log_writers[i].reset();
                handle.log_health.record_failure(now, 0, sibling_error);
            }
        }
        bool all_writers_open = true;
        for (int i = 0; i < writer_count; ++i)
            all_writers_open = all_writers_open && !!handle.log_writers[i];
        if (handle.log_health.degraded && all_writers_open) {
            daemon_log_message(daemon_log_level_t::info, "Program `" + handle.config.name + "` log recovered.");
            handle.log_health.record_recovery();
        }
    }
    if (!handle.log_writers[writer_index]) { handle.log_health.dropped_bytes += size; return; }
    std::string error;
    if (!handle.log_writers[writer_index]->append(data, size, error)) {
        handle.log_writers[writer_index].reset();
        const bool changed = !handle.log_health.degraded || handle.log_health.last_error != error;
        handle.log_health.record_failure(now, size, error);
        if (changed) daemon_log_message(daemon_log_level_t::error, "Program `" + handle.config.name + "` log degraded: " + error);
    }
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
    const auto snapshot = state.dependencies.snapshot();
    state.graph = std::move(graph);
    state.dependencies.migrate(state.graph, snapshot);
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
    const auto action = handle.termination.request(handle.generation,
                                                   static_cast<std::int64_t>(monotonic_millis()),
                                                   handle.config.kill_timeout_s);
    if (action == termination_action::none) return true;
    if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, handle.proc_info.dwProcessId)) {
        return true;
    }

    const auto signal_error = GetLastError();
    handle.termination.force(handle.generation);
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
    const auto action = handle.termination.poll(handle.generation, static_cast<std::int64_t>(now_ms), tree_empty);
    if (action != termination_action::send_kill) return true;
    return force_terminate_program(handle, handle.termination_exit_code, error_message);
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
            daemon_log_message(daemon_log_level_t::error, terminate_error);
        }
        WaitForSingleObject(proc.proc_info.hProcess, wait_ms);
        proc.reset();
    }
    processes.clear();
}

} // namespace win
} // namespace pm_tiny
