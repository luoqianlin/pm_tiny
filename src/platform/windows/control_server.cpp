#include "control_server.h"

#include "core/prog_cfg_yaml_helper.h"
#include "core/control_command.h"

#include "process_runner.h"
#include "win_config_loader.h"
#include "win_utils.h"
#include "frame_stream.hpp"
#include "asio_named_pipe.h"
#include "process_list.h"
#include "pm_tiny.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>
#include <cstdio>

#include <yaml-cpp/yaml.h>

#include <windows.h>
#include <psapi.h>

namespace pm_tiny {
namespace win {

namespace {
struct StartArgs {
    std::string name;
    std::string command;
};

StartArgs parse_start_args(const std::string &args) {
    StartArgs result;
    auto name_pos = args.find_first_of(" \t");
    result.name = (name_pos == std::string::npos) ? trim_copy(args) : trim_copy(args.substr(0, name_pos));
    result.command = (name_pos == std::string::npos) ? std::string() : trim_copy(args.substr(name_pos + 1));
    return result;
}
} // namespace

ControlServer::ControlServer(std::vector<ProcessHandle> &processes,
                             std::mutex &process_mutex,
                             std::unordered_map<std::string, ProgramConfig> &config_map,
                             RuntimeControlState &runtime_state,
                             std::atomic_bool &should_stop_flag,
                             std::string config_path)
        : pipe_name_(control_pipe_name_wide()),
          processes_(processes),
          process_mutex_(process_mutex),
          config_map_(config_map),
          runtime_state_(runtime_state),
          should_stop_(should_stop_flag),
          config_path_(std::move(config_path)) {}

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start() {
    if (running_.exchange(true)) {
        return true;
    }
    try {
        thread_ = std::thread(&ControlServer::run, this);
    } catch (const std::exception &ex) {
        std::cerr << "[ERROR] Failed to start control server thread: " << ex.what() << std::endl;
        running_.store(false);
        return false;
    }
    return true;
}

void ControlServer::stop() {
    bool was_running = running_.exchange(false);
    if (was_running) {
        HANDLE pipe = CreateFileW(pipe_name_.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  0,
                                  nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pm_tiny::protocol_message msg;
            msg.type = 0x35;
            msg.request_id = 1;
            auto encoded = pm_tiny::protocol_encode(msg);
            DWORD written = 0;
            WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr);
            CloseHandle(pipe);
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ControlServer::run() {
    while (running_.load()) {
        std::string pipe_error;
        HANDLE pipe = AsioNamedPipe::accept(pipe_name_, running_, pipe_error);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (running_.load() && !pipe_error.empty()) std::cerr << "[ERROR] " << pipe_error << std::endl;
            continue;
        }
        AsioNamedPipe connection(pipe);
        pm_tiny::protocol_message decoded_request;
        if (!connection.read_message(decoded_request, std::chrono::seconds(5), pipe_error)) {
            if (running_.load()) std::cerr << "[ERROR] " << pipe_error << std::endl;
            continue;
        }
        auto response = handle_message(decoded_request);
        std::vector<pm_tiny::protocol_message> responses;
            if (decoded_request.type == 0x30 && !(response.flags & pm_tiny::protocol_flag_error)) {
                std::int32_t status = -1;
                std::string content;
                pm_tiny::iframe_stream response_stream(response.payload);
                response_stream >> status >> content;
                response.payload.clear();
                pm_tiny::fappend_value<std::int32_t>(response.payload, 0);
                pm_tiny::fappend_value(response.payload, std::string("OK"));
                responses.push_back(response);
                auto chunks = pm_tiny::str_to_frames(1, content);
                for (std::size_t i = 0; i < chunks.size(); ++i) {
                    pm_tiny::protocol_message stream_message;
                    stream_message.type = decoded_request.type;
                    stream_message.request_id = decoded_request.request_id;
                    stream_message.flags = static_cast<std::uint8_t>(pm_tiny::protocol_flag_response |
                            pm_tiny::protocol_flag_stream |
                            (i + 1 < chunks.size() ? pm_tiny::protocol_flag_more : 0));
                    stream_message.payload = *chunks[i];
                    responses.push_back(stream_message);
                }
            } else {
                responses.push_back(response);
            }
        for (const auto &item : responses) {
            if (!connection.write_message(item, std::chrono::seconds(5), pipe_error)) {
                if (running_.load()) std::cerr << "[ERROR] " << pipe_error << std::endl;
                break;
            }
        }
    }
}

pm_tiny::protocol_message ControlServer::handle_message(const pm_tiny::protocol_message &request) {
    const auto decoded = decode_control_request(request);
    if (!decoded.success) return make_control_response(request, -1, "ERR " + decoded.error + "\n");
    if (decoded.command == control_command::list) return handle_list(request);
    const auto &args = decoded.name;
    std::string result;
    switch (decoded.command) {
        case control_command::stop: result = handle_stop(args); break;
        case control_command::start: result = handle_start(args); break;
        case control_command::restart: result = handle_restart(args); break;
        case control_command::save: result = handle_save(); break;
        case control_command::remove: result = handle_delete(args); break;
        case control_command::version: result = "pm_tiny Windows v2"; break;
        case control_command::log: result = handle_log(args); break;
        case control_command::inspect: result = handle_inspect(args); break;
        case control_command::reload: result = handle_reload(); break;
        case control_command::app_ready: result = handle_app_signal(args, true); break;
        case control_command::app_tick: result = handle_app_signal(args, false); break;
        case control_command::quit:
            should_stop_.store(true);
            running_.store(false);
            result = "OK quitting\n";
            break;
        case control_command::list: break;
    }
    const bool error = result.rfind("ERR", 0) == 0;
    return make_control_response(request, error ? -1 : 0, result);
}

pm_tiny::protocol_message ControlServer::handle_list(const pm_tiny::protocol_message &request) {
    pm_tiny::protocol_message response;
    response.type = request.type;
    response.request_id = request.request_id;
    response.flags = pm_tiny::protocol_flag_response;
    pm_tiny::fappend_value<std::int32_t>(response.payload, 0);
    pm_tiny::fappend_value(response.payload, "success");
    std::vector<pm_tiny::process_list_entry> entries;
    std::lock_guard<std::mutex> lock(process_mutex_);
    entries.reserve(processes_.size());
    const auto now_ms = monotonic_millis();
    for (const auto &proc : processes_) {
        pm_tiny::process_list_entry entry;
        DWORD exit_code = 0;
        const bool active = proc.has_process && proc.proc_info.hProcess != nullptr &&
            GetExitCodeProcess(proc.proc_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE;
        entry.name = proc.config.name;
        entry.cwd = proc.config.cwd;
        entry.command = proc.config.command;
        entry.restart_count = proc.restart_count;
        if (proc.termination_phase != TerminationPhase::none) entry.state = PM_TINY_PROG_STATE_REQUEST_STOP;
        else if (active) entry.state = proc.ready || proc.config.start_timeout == 0 ?
                                        PM_TINY_PROG_STATE_RUNING : PM_TINY_PROG_STATE_STARTING;
        else if (proc.restart_pending) entry.state = PM_TINY_PROG_STATE_WAITING_START;
        else if (runtime_state_.dependencies.state(proc.config.name) == dependency_runtime_state::blocked)
            entry.state = PM_TINY_PROG_STATE_BLOCKED;
        else if (runtime_state_.dependencies.state(proc.config.name) == dependency_runtime_state::failed)
            entry.state = PM_TINY_PROG_STATE_STARTUP_FAIL;
        else if (runtime_state_.dependencies.state(proc.config.name) == dependency_runtime_state::pending)
            entry.state = PM_TINY_PROG_STATE_WAITING_START;
        else entry.state = PM_TINY_PROG_STATE_STOPED;
        if (active) {
            entry.pid = static_cast<std::int64_t>(proc.proc_info.dwProcessId);
            entry.has_uptime = proc.launch_time_ms > 0;
            if (entry.has_uptime) entry.uptime_ms = static_cast<std::int64_t>(now_ms - proc.launch_time_ms);
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(proc.proc_info.hProcess, &counters, sizeof(counters))) {
                entry.has_rss = true;
                entry.rss_kib = static_cast<std::int64_t>(counters.WorkingSetSize / 1024ULL);
            }
        }
        entry.daemon = proc.config.daemon;
        entry.pty = pm_tiny::pty_mode_t::unsupported;
        entry.depends_on = proc.config.depends_on;
        entry.restart_pending = proc.restart_pending;
        if (entry.restart_pending) {
            entry.restart_delay_remaining_ms = static_cast<std::int64_t>(
                proc.restart_due_ms > now_ms ? proc.restart_due_ms - now_ms : 0);
        }
        entry.restart_attempts_in_window = static_cast<std::int32_t>(proc.restart_state.attempts_ms.size());
        entry.restart_suppressed = proc.restart_state.suppressed;
        if (entry.restart_suppressed) entry.restart_suppression_reason = pm_tiny::restart_attempt_limit_reason;
        entries.push_back(std::move(entry));
    }
    pm_tiny::append_process_list(response.payload, entries);
    return response;
}

std::string ControlServer::handle_stop(const std::string &name) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &proc) {
        return proc.config.name == name;
    });
    if (it == processes_.end()) {
        return "ERR process not found\n";
    }
    ProcessHandle &proc = *it;
    if (!proc.has_process || proc.proc_info.hProcess == nullptr) {
        runtime_state_.dependencies.mark_idle(name);
        return "OK already stopped\n";
    }
    proc.disable_restart = true;
    proc.restart_state.reset();
    std::string terminate_error;
    if (!request_program_termination(proc, CompletionAction::remove, 0, terminate_error)) {
        return "ERR " + terminate_error + "\n";
    }
    if (!terminate_error.empty()) std::cerr << "[WARN] " << proc.config.name << ": " << terminate_error << std::endl;
    return "OK stop requested\n";
}

std::string ControlServer::handle_restart(const std::string &name) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    if (runtime_state_.reload_pending || should_stop_.load()) return "ERR daemon is busy\n";
    auto it = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &proc) {
        return proc.config.name == name;
    });
    if (it == processes_.end()) return "ERR process not found\n";
    it->disable_restart = false;
    it->restart_state.reset();
    if (!it->has_process || it->proc_info.hProcess == nullptr) {
        const auto failures = schedule_dependency_launch(
            processes_, runtime_state_, runtime_state_.dependencies.request_closure(name));
        if (!failures.empty()) return "ERR failed to restart: " + failures.front() + "\n";
        return "OK restart scheduled\n";
    }
    std::string terminate_error;
    if (!request_program_termination(*it, CompletionAction::restart, 0, terminate_error)) {
        return "ERR " + terminate_error + "\n";
    }
    if (!terminate_error.empty()) std::cerr << "[WARN] " << it->config.name << ": " << terminate_error << std::endl;
    return "OK restart requested\n";
}

std::string ControlServer::handle_start(const std::string &args) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    if (runtime_state_.reload_pending || should_stop_.load()) return "ERR daemon is busy\n";
    auto parsed = parse_start_args(args);
    if (parsed.name.empty()) {
        return "ERR missing process name\n";
    }
    auto cfg_iter = config_map_.find(parsed.name);
    bool has_config = cfg_iter != config_map_.end();
    if (!has_config && parsed.command.empty()) {
        return "ERR missing command\n";
    }
    ProgramConfig cfg = has_config ? cfg_iter->second : ProgramConfig{};
    cfg.name = parsed.name;
    if (!parsed.command.empty()) {
        cfg.command = parsed.command;
    }
    if (cfg.command.empty()) {
        return "ERR missing command\n";
    }
    if (cfg.cwd.empty()) {
        cfg.cwd = ".";
    }
    if (cfg.log_dir.empty()) {
        cfg.log_dir = "logs";
    }
    if (cfg.log_file_name.empty()) {
        cfg.log_file_name = cfg.name + ".log";
    }
    std::vector<ProgramConfig> configs;
    configs.reserve(config_map_.size() + 1);
    for (const auto &entry : config_map_) if (entry.first != cfg.name) configs.push_back(entry.second);
    configs.push_back(cfg);
    std::string dependency_error;
    if (!rebuild_dependencies(runtime_state_, configs, dependency_error)) return "ERR " + dependency_error + "\n";
    for (const auto &process : processes_) {
        if (process.has_process && (process.ready || process.config.start_timeout == 0))
            runtime_state_.dependencies.mark_ready(process.config.name);
    }
    config_map_[cfg.name] = cfg;
    ensure_process_records(processes_, configs);
    const auto failures = schedule_dependency_launch(
        processes_, runtime_state_, runtime_state_.dependencies.request_closure(cfg.name));
    if (!failures.empty()) return "ERR failed to start: " + failures.front() + "\n";
    return "OK started\n";
}

std::string ControlServer::handle_delete(const std::string &name) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    const auto id = runtime_state_.graph.find(name);
    if (id != dependency_graph::npos && !runtime_state_.graph.dependents(id).empty()) {
        std::string dependents;
        for (const auto dependent : runtime_state_.graph.dependents(id)) {
            if (!dependents.empty()) dependents += ",";
            dependents += runtime_state_.graph.name(dependent);
        }
        return "ERR cannot delete `" + name + "`; required by: " + dependents + "\n";
    }
    auto it = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &proc) {
        return proc.config.name == name;
    });
    const bool had_process_record = it != processes_.end();
    if (it != processes_.end()) {
        it->disable_restart = true;
        if (it->has_process && it->proc_info.hProcess != nullptr) {
            std::string terminate_error;
            if (!request_program_termination(*it, CompletionAction::delete_config, 0, terminate_error)) {
                return "ERR " + terminate_error + "\n";
            }
            if (!terminate_error.empty()) std::cerr << "[WARN] " << it->config.name << ": " << terminate_error << std::endl;
            config_map_.erase(name);
            std::vector<ProgramConfig> configs;
            for (const auto &entry : config_map_) configs.push_back(entry.second);
            std::string error;
            rebuild_dependencies(runtime_state_, configs, error);
            return "OK deleted\n";
        } else {
            it->reset();
            processes_.erase(it);
            config_map_.erase(name);
            std::vector<ProgramConfig> configs;
            for (const auto &entry : config_map_) configs.push_back(entry.second);
            std::string error;
            rebuild_dependencies(runtime_state_, configs, error);
        }
    }
    if (config_map_.erase(name) == 0 && !had_process_record) return "ERR process not found\n";
    return "OK deleted\n";
}

std::string ControlServer::handle_inspect(const std::string &name) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = config_map_.find(name);
    if (it == config_map_.end()) return "ERR process not found\n";
    const auto &cfg = it->second;
    const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &candidate) {
        return candidate.config.name == name;
    });
    const bool restart_pending = process != processes_.end() && process->restart_pending;
    const bool restart_suppressed = process != processes_.end() && process->restart_state.suppressed;
    const auto now_ms = monotonic_millis();
    const auto remaining_ms = restart_pending && process->restart_due_ms > now_ms ?
        process->restart_due_ms - now_ms : 0;
    const auto attempts = process == processes_.end() ? 0 : process->restart_state.attempts_ms.size();
    const auto dependency_state = runtime_state_.dependencies.state(name);
    std::string dependency_state_text = "idle";
    switch (dependency_state) {
        case dependency_runtime_state::pending: dependency_state_text = "pending"; break;
        case dependency_runtime_state::starting: dependency_state_text = "starting"; break;
        case dependency_runtime_state::ready: dependency_state_text = "ready"; break;
        case dependency_runtime_state::failed: dependency_state_text = "failed"; break;
        case dependency_runtime_state::blocked: dependency_state_text = "blocked"; break;
        case dependency_runtime_state::idle: break;
    }
    std::string blockers;
    for (const auto &blocker : runtime_state_.dependencies.blocked_by(name)) {
        if (!blockers.empty()) blockers += ",";
        blockers += blocker;
    }
    std::ostringstream out;
    out << "name=" << cfg.name << "\ncommand=" << cfg.command << "\ncwd=" << cfg.cwd
        << "\ndaemon=" << (cfg.daemon ? "true" : "false")
        << "\nkill_timeout=" << cfg.kill_timeout_s
        << "\nstart_timeout=" << cfg.start_timeout
        << "\nheartbeat_timeout=" << cfg.heartbeat_timeout
        << "\nrestart_delay_ms=" << cfg.restart_delay_ms
        << "\nrestart_max_delay_ms=" << cfg.restart_max_delay_ms
        << "\nrestart_window_ms=" << cfg.restart_window_ms
        << "\nrestart_max_attempts=" << cfg.restart_max_attempts
        << "\nrestart_reset_after_ms=" << cfg.restart_reset_after_ms
        << "\nrestart_pending=" << (restart_pending ? "true" : "false")
        << "\nrestart_delay_remaining_ms=" << remaining_ms
        << "\nrestart_attempts_in_window=" << attempts
        << "\nrestart_suppressed=" << (restart_suppressed ? "true" : "false")
        << "\nrestart_suppression_reason="
        << (restart_suppressed ? pm_tiny::restart_attempt_limit_reason : "")
        << "\ndependency_state=" << dependency_state_text
        << "\nblocked_by=" << blockers << "\n";
    return out.str();
}

std::string ControlServer::handle_log(const std::string &name) {
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = config_map_.find(name);
    if (it == config_map_.end()) return "ERR process not found\n";
    const auto &cfg = it->second;
    const auto file_name = cfg.log_file_name.empty() ? cfg.name + ".log" : cfg.log_file_name;
    const auto path = (cfg.log_dir.empty() ? std::string("logs") : cfg.log_dir) + "\\" + file_name;
    std::ifstream input(path, std::ios::binary);
    if (!input) return "ERR log file not found\n";
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string ControlServer::handle_save() {
    std::lock_guard<std::mutex> lock(process_mutex_);
    if (runtime_state_.reload_pending || should_stop_.load()) return "ERR daemon is busy\n";
    YAML::Emitter out;
    out << YAML::BeginSeq;
    for (const auto &entry : config_map_) {
        const auto &cfg = entry.second;
        ProgCfgSerializeOptions serialize_options;
        serialize_options.include_run_as = false;
        serialize_options.include_oom_score_adj = false;
        serialize_options.include_pty = false;
        YAML::Node node = serialize_prog_cfg_yaml_node(cfg, serialize_options);
        node["log_dir"] = cfg.log_dir;
        node["log_file_name"] = cfg.log_file_name;
        node["log_max_size_kb"] = cfg.log_max_size_kb;
        node["log_file_count"] = cfg.log_file_count;
        out << node;
    }
    out << YAML::EndSeq;
    const auto temporary = config_path_ + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return "ERR cannot open temporary config\n";
        file << out.c_str() << "\n";
        if (!file) return "ERR cannot write temporary config\n";
    }
    if (!MoveFileExA(temporary.c_str(), config_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(temporary.c_str());
        return "ERR cannot replace config file\n";
    }
    return "OK saved\n";
}

std::string ControlServer::handle_reload() {
    auto loaded = load_program_configs(config_path_);
    if (!loaded.error_message.empty()) return "ERR " + loaded.error_message + "\n";
    std::unique_lock<std::mutex> lock(process_mutex_);
    if (runtime_state_.reload_pending || should_stop_.load()) return "ERR daemon is busy\n";
    runtime_state_.reload_pending = true;
    runtime_state_.reload_programs = std::move(loaded.programs);
    for (auto iter = processes_.rbegin(); iter != processes_.rend(); ++iter) {
        iter->disable_restart = true;
        std::string error;
        if (!request_program_termination(*iter, CompletionAction::remove, 0, error)) {
            std::cerr << "[ERROR] Failed to stop `" << iter->config.name << "` for reload: " << error << std::endl;
        } else if (!error.empty()) {
            std::cerr << "[WARN] " << iter->config.name << ": " << error << std::endl;
        }
    }
    runtime_state_.reload_completed.wait(lock, [&]() {
        return !runtime_state_.reload_pending || should_stop_.load();
    });
    if (should_stop_.load()) return "ERR daemon is shutting down\n";
    return "OK reloaded\n";
}

std::string ControlServer::handle_app_signal(const std::string &name, bool ready_signal) {
    if (name.empty()) return "ERR missing process name\n";
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto found = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &process) {
        return process.config.name == name;
    });
    if (found == processes_.end() || !found->has_process) return "ERR process not found\n";
    found->last_tick_ms = monotonic_millis();
    if (ready_signal) {
        found->ready = true;
        const auto failures = schedule_dependency_launch(
            processes_, runtime_state_, runtime_state_.dependencies.mark_ready(name));
        if (!failures.empty()) return "ERR failed to start dependency: " + failures.front() + "\n";
    }
    return ready_signal ? "OK ready\n" : "OK tick\n";
}

} // namespace win
} // namespace pm_tiny
