#include "control_server.h"
#include "daemon_log.h"

#include "core/control_command.h"

#include "process_runner.h"
#include "control_operation.h"
#include "runtime_snapshot.h"
#include "win_config_loader.h"
#include "win_utils.h"
#include "frame_stream.hpp"
#include "asio_named_pipe.h"
#include "process_list.h"
#include "pm_tiny.h"
#include "windows_program_persistence.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <fstream>
#include <cstdio>

#include <windows.h>
#include <psapi.h>

namespace pm_tiny {
namespace win {

struct ControlServer::pending_response {
    std::weak_ptr<AsyncNamedPipeSession> session;
    protocol_message request;
    std::string name;
    control_command command = control_command::list;
    unsigned long long generation = 0;
    bool show_log = false;
};

struct ControlServer::log_stream {
    std::weak_ptr<AsyncNamedPipeSession> session;
    protocol_message request;
    std::string name;
    unsigned long long generation = 0;
    unsigned long long offset = 0;
    bool follow_process = false;
    bool waiting_for_generation = false;
    log_request_mode mode = log_request_mode::live;
};

namespace {

struct delete_plan {
    std::vector<ProgramConfig> remaining_configs;
    dependency_graph remaining_graph;
};

bool prepare_delete(const std::string &name,
                    const std::map<std::string, ProgramConfig> &config_map,
                    const RuntimeControlState &runtime_state,
                    delete_plan &plan,
                    std::string &error_message) {
    if (config_map.find(name) == config_map.end()) {
        error_message = "process not found";
        return false;
    }
    const auto id = runtime_state.graph.find(name);
    if (id != dependency_graph::npos && !runtime_state.graph.dependents(id).empty()) {
        std::string dependents;
        for (const auto dependent : runtime_state.graph.dependents(id)) {
            if (!dependents.empty()) dependents += ",";
            dependents += runtime_state.graph.name(dependent);
        }
        error_message = "cannot delete `" + name + "`; required by: " + dependents;
        return false;
    }
    plan.remaining_configs.clear();
    plan.remaining_configs.reserve(config_map.size() - 1);
    for (const auto &entry : config_map) {
        if (entry.first != name) plan.remaining_configs.push_back(entry.second);
    }
    return build_dependencies(plan.remaining_configs, plan.remaining_graph, error_message);
}

void commit_delete(const std::string &name,
                   std::map<std::string, ProgramConfig> &config_map,
                   RuntimeControlState &runtime_state,
                   delete_plan plan) {
    config_map.erase(name);
    replace_dependencies(runtime_state, std::move(plan.remaining_graph));
}

void apply_manual_stop(ProcessHandle &process, RuntimeControlState &runtime_state) {
    process.disable_restart = true;
    process.restart_pending = false;
    process.restart_due_ms = 0;
    process.restart_state.reset();
    runtime_state.dependencies.mark_idle(process.config.name);
}

runtime_snapshot snapshot_runtime(const ProcessHandle &process,
                                  const RuntimeControlState &runtime_state,
                                  unsigned long long now_ms) {
    runtime_snapshot snapshot;
    DWORD exit_code = 0;
    const bool active = process.has_process && process.proc_info.hProcess != nullptr &&
        GetExitCodeProcess(process.proc_info.hProcess, &exit_code) && exit_code == STILL_ACTIVE;
    snapshot.pid = active ? static_cast<std::int64_t>(process.proc_info.dwProcessId) : -1;
    snapshot.generation = process.generation;
    snapshot.restart_count = process.restart_count;
    snapshot.ready = active && (process.ready || process.config.start_timeout == 0);
    snapshot.heartbeat_enabled = process.config.heartbeat_timeout > 0;
    snapshot.has_last_tick_age = snapshot.heartbeat_enabled && process.last_tick_ms > 0;
    if (snapshot.has_last_tick_age)
        snapshot.last_tick_age_ms = static_cast<std::int64_t>(now_ms - process.last_tick_ms);
    snapshot.has_uptime = active && process.launch_time_ms > 0;
    if (snapshot.has_uptime)
        snapshot.uptime_ms = static_cast<std::int64_t>(now_ms - process.launch_time_ms);
    if (active) {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(process.proc_info.hProcess, &counters, sizeof(counters))) {
            snapshot.has_rss = true;
            snapshot.rss_kib = static_cast<std::int64_t>(counters.WorkingSetSize / 1024ULL);
        }
    }
    if (process.termination.phase() != termination_phase::none || (process.has_process && !active))
        snapshot.state = PM_TINY_PROG_STATE_REQUEST_STOP;
    else if (active) snapshot.state = snapshot.ready ? PM_TINY_PROG_STATE_RUNING : PM_TINY_PROG_STATE_STARTING;
    else if (process.restart_pending) snapshot.state = PM_TINY_PROG_STATE_WAITING_START;
    else if (runtime_state.dependencies.state(process.config.name) == dependency_runtime_state::blocked)
        snapshot.state = PM_TINY_PROG_STATE_BLOCKED;
    else if (runtime_state.dependencies.state(process.config.name) == dependency_runtime_state::failed)
        snapshot.state = PM_TINY_PROG_STATE_STARTUP_FAIL;
    else if (runtime_state.dependencies.state(process.config.name) == dependency_runtime_state::pending)
        snapshot.state = PM_TINY_PROG_STATE_WAITING_START;
    else snapshot.state = PM_TINY_PROG_STATE_STOPED;
    snapshot.pty = pty_mode_t::unsupported;
    snapshot.restart_pending = process.restart_pending;
    snapshot.restart_delay_remaining_ms = process.restart_pending && process.restart_due_ms > now_ms ?
        static_cast<std::int64_t>(process.restart_due_ms - now_ms) : 0;
    snapshot.restart_attempts_in_window = static_cast<std::int32_t>(process.restart_state.attempts_ms.size());
    snapshot.restart_suppressed = process.restart_state.suppressed;
    if (snapshot.restart_suppressed) snapshot.restart_suppression_reason = restart_attempt_limit_reason;
    snapshot.has_last_exit = process.has_last_exit;
    if (snapshot.has_last_exit) {
        snapshot.last_exit_reason = exit_reason_t::exited;
        snapshot.last_exit_code = static_cast<std::int32_t>(process.last_exit_code);
    }
    snapshot.process_tree_backend = "job_object";
    snapshot.config_source = process.config_source;
    snapshot.log_degraded = process.log_health.degraded;
    snapshot.log_dropped_bytes = process.log_health.dropped_bytes;
    snapshot.log_last_error = process.log_health.last_error;
    snapshot.log_retry_remaining_ms = process.log_health.degraded && process.log_health.retry_due_ms >
        static_cast<std::int64_t>(now_ms) ? process.log_health.retry_due_ms - static_cast<std::int64_t>(now_ms) : 0;
    snapshot.log_paths = process.log_paths;
    return snapshot;
}

} // namespace

ControlServer::ControlServer(std::vector<ProcessHandle> &processes,
                             std::map<std::string, ProgramConfig> &config_map,
                             RuntimeControlState &runtime_state,
                             std::atomic_bool &should_stop_flag,
                             std::string program_config_path,
                             std::string app_environ_dir,
                             std::string app_log_dir,
                             daemon_config daemon_configuration,
                             daemon_cli_options daemon_options,
                             unsigned long long started_ms)
        : pipe_name_(control_pipe_name_wide()),
          processes_(processes),
          config_map_(config_map),
          runtime_state_(runtime_state),
          should_stop_(should_stop_flag),
          program_config_path_(std::move(program_config_path)),
          app_environ_dir_(std::move(app_environ_dir)),
          app_log_dir_(std::move(app_log_dir)),
          daemon_configuration_(std::move(daemon_configuration)),
          daemon_options_(std::move(daemon_options)),
          started_ms_(started_ms) {}

ControlServer::~ControlServer() {
    stop();
}

bool ControlServer::start() {
    if (running_.exchange(true)) {
        return true;
    }
    server_.reset(new AsyncNamedPipeServer(pipe_name_,
        [this](const std::shared_ptr<AsyncNamedPipeSession> &session, const protocol_message &request) {
            handle_async_request(session, request);
        }, {}, &running_));
    std::string error;
    if (!server_->start(error)) {
        daemon_log_message(daemon_log_level_t::error, "Failed to start control server: " + error);
        server_.reset();
        running_.store(false);
        return false;
    }
    return true;
}

void ControlServer::stop() {
    running_.store(false);
    persistence_.wait();
    if (server_) server_->stop();
    server_.reset();
}

void ControlServer::poll() {
    poll_pending_responses();
    if (!server_) return;
    std::string error;
    if (!server_->poll(error) && !error.empty()) {
        daemon_log_message(daemon_log_level_t::error, error);
        running_.store(false);
    }
    refresh_process_exit_watches();
    poll_log_streams();
}

void ControlServer::run_for(unsigned long milliseconds) {
    poll_pending_responses();
    if (!server_) return;
    std::string error;
    server_->run_for(milliseconds, error);
    if (!error.empty()) daemon_log_message(daemon_log_level_t::error, error);
    refresh_process_exit_watches();
    poll_log_streams();
}

bool ControlServer::persistence_busy() const {
    return persistence_.busy();
}

void ControlServer::refresh_process_exit_watches() {
    if (!server_) return;
    for (auto &process : processes_) {
        if (!process.has_process || process.proc_info.hProcess == nullptr) continue;
        std::string error;
        if (process.watched_generation != process.generation) {
            if (!server_->watch_process(
                    process.proc_info.hProcess, process.config.name, process.generation,
                    [this](const std::string &name, unsigned long long generation, unsigned long exit_code) {
                        handle_process_exit(name, generation, exit_code);
                    }, error)) {
                daemon_log_message(daemon_log_level_t::error, "Failed to watch process `" + process.config.name + "`: " + error);
            } else {
                process.watched_generation = process.generation;
            }
        }
        const int stream_count = process.config.log_mode == log_mode_t::split ? 2 : 1;
        for (int stream = 0; stream < stream_count; ++stream) {
        if (process.pipe_read[stream] != nullptr && process.watched_log_generation[stream] != process.generation) {
            HANDLE pipe = process.pipe_read[stream];
            if (!server_->watch_process_log(
                    pipe, process.config.name, process.generation,
                    [this, stream](const std::string &name, unsigned long long generation,
                           const char *data, std::size_t size, bool eof) {
                        handle_process_log(name, generation, stream, data, size, eof);
                    }, error)) {
                daemon_log_message(daemon_log_level_t::error, "Failed to watch log pipe for `" + process.config.name + "`: " + error);
            } else {
                process.pipe_read[stream] = nullptr;
                process.watched_log_generation[stream] = process.generation;
            }
        }
        }
    }
}

void ControlServer::handle_process_log(const std::string &name, unsigned long long generation, int stream_index,
                                       const char *data, std::size_t size, bool eof) {
    auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == name && item.generation == generation;
    });
    if (process == processes_.end()) return;
    if (size > 0) append_program_log(*process, stream_index, data, size);
    if (eof) process->log_pipe_eof[stream_index] = true;
}

void ControlServer::handle_process_exit(const std::string &name, unsigned long long generation,
                                        unsigned long exit_code) {
    auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == name && item.generation == generation;
    });
    if (process == processes_.end()) return;
    process->root_exit_observed = true;
    process->root_exit_code = exit_code;
    process->has_last_exit = true;
    process->last_exit_code = exit_code;
    process->last_exit_time_unix_ms = current_unix_time_millis();
}

void ControlServer::handle_async_request(const std::shared_ptr<AsyncNamedPipeSession> &session,
                                         const protocol_message &request) {
    const auto decoded = decode_control_request(request);
    if (!decoded.success) {
        session->send(make_control_response(request, -1, "ERR " + decoded.error + "\n"));
        session->finish();
        return;
    }
    if (decoded.command == control_command::log) {
        try {
            const auto log_request = read_program_log_request(request.payload);
            const auto process = std::find_if(processes_.begin(), processes_.end(),
                [&](const ProcessHandle &item) { return item.config.name == log_request.name; });
            const bool wait_for_restart = log_request.mode == log_request_mode::live &&
                process != processes_.end() && !process->has_process && process->restart_pending;
            begin_log_stream(session, request, log_request.name, wait_for_restart, log_request.mode);
        } catch (const std::exception &error) {
            session->send(make_control_response(request, -1, std::string("ERR ") + error.what() + "\n"));
            session->finish();
        }
        return;
    }
    if (decoded.command == control_command::reload) {
        if (persistence_.busy()) {
            session->send(make_control_response(request, -1, "ERR persistence operation busy\n"));
            session->finish();
            return;
        }
        auto loaded = load_program_configs(program_config_path_, app_environ_dir_);
        if (!loaded.error_message.empty()) {
            session->send(make_control_response(request, -1, "ERR " + loaded.error_message + "\n"));
            session->finish();
            return;
        }
        if (runtime_state_.reload_pending || should_stop_.load()) {
            session->send(make_control_response(request, -1, "ERR daemon is busy\n"));
            session->finish();
            return;
        }
        runtime_state_.reload_pending = true;
        runtime_state_.reload_programs = std::move(loaded.programs);
        for (auto iter = processes_.rbegin(); iter != processes_.rend(); ++iter) {
            iter->disable_restart = true;
            std::string error;
            request_program_termination(*iter, CompletionAction::remove, 0, error);
        }
        pending_responses_.push_back({session, request, {}, decoded.command, 0, false});
        return;
    }
    if (decoded.command == control_command::save) {
        if (runtime_state_.reload_pending || should_stop_.load() || persistence_.busy()) {
            session->send(make_control_response(request, -1, "ERR persistence operation busy\n"));
            session->finish();
            return;
        }
        std::vector<ProgramConfig> configs;
        configs.reserve(config_map_.size());
        for (const auto &entry : config_map_) {
            configs.push_back(entry.second);
        }
        const std::string config_path = program_config_path_;
        const std::string environ_path = app_environ_dir_;
        if (!persistence_.submit([configs, config_path, environ_path]() {
            std::string error;
            const int result = save_program_configs(configs, config_path, environ_path, error);
            if (result != 0 && !error.empty()) daemon_log_message(daemon_log_level_t::error, error);
            return result;
        })) {
            session->send(make_control_response(request, -1, "ERR persistence operation busy\n"));
            session->finish();
            return;
        }
        pending_responses_.push_back({session, request, {}, decoded.command, 0, false});
        return;
    }
    if (decoded.command != control_command::stop && decoded.command != control_command::restart &&
        decoded.command != control_command::remove) {
        auto response = handle_message(request);
        session->send(response);
        if (decoded.command == control_command::start) {
            try {
                const auto start_request_value = read_start_request(request.payload);
                if (start_request_value.show_log && !(response.flags & protocol_flag_error)) {
                    begin_log_stream(session, request, start_request_value.name, true);
                    return;
                }
            } catch (...) {}
        }
        session->finish();
        return;
    }
    auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == decoded.name;
    });
    if (process == processes_.end()) {
        session->send(make_control_response(request, -1, "ERR process not found\n"));
        session->finish();
        return;
    }
    std::string error;
    bool show_log = false;
    if (decoded.command == control_command::restart) {
        try {
            iframe_stream restart_payload(request.payload);
            std::string ignored_name;
            std::int32_t value = 0;
            restart_payload >> ignored_name;
            if (restart_payload.remaining_size() > 0) restart_payload >> value;
            show_log = value != 0;
        } catch (...) {}
    }
    CompletionAction action = CompletionAction::remove;
    if (decoded.command == control_command::restart) action = CompletionAction::restart;
    if (decoded.command == control_command::remove) action = CompletionAction::delete_config;
    delete_plan deletion;
    if (decoded.command == control_command::remove &&
        !prepare_delete(decoded.name, config_map_, runtime_state_, deletion, error)) {
        session->send(make_control_response(request, -1, "ERR " + error + "\n"));
        session->finish();
        return;
    }
    if (process->has_process && !request_program_termination(*process, action, 0, error)) {
        session->send(make_control_response(request, -1, "ERR " + error + "\n"));
        session->finish();
        return;
    }
    if (decoded.command == control_command::remove) {
        commit_delete(decoded.name, config_map_, runtime_state_, std::move(deletion));
    }
    if (decoded.command == control_command::stop) {
        apply_manual_stop(*process, runtime_state_);
    }
    if (!process->has_process) {
        if (decoded.command == control_command::stop) {
            session->send(make_control_response(request, 0, "OK already stopped\n"));
        } else if (decoded.command == control_command::remove) {
            processes_.erase(process);
            session->send(make_control_response(request, 0, "OK deleted\n"));
        } else {
            const auto previous_generation = process->generation;
            const auto failures = schedule_dependency_launch(
                processes_, runtime_state_, runtime_state_.dependencies.request_closure(decoded.name));
            if (!failures.empty()) {
                session->send(make_control_response(request, -1,
                    "ERR failed to restart: " + failures.front() + "\n"));
                session->finish();
                return;
            }
            if (show_log) {
                pending_responses_.push_back({session, request, decoded.name, decoded.command,
                                              previous_generation, true});
                return;
            }
            session->send(make_control_response(request, 0, "OK restart scheduled\n"));
        }
        session->finish();
        return;
    }
    pending_responses_.push_back({session, request, decoded.name, decoded.command,
                                  process->generation, show_log});
}

void ControlServer::poll_pending_responses() {
    int save_result = -1;
    const bool save_completed = persistence_.poll(save_result);
    for (auto it = pending_responses_.begin(); it != pending_responses_.end();) {
        auto session = it->session.lock();
        if (!session) { it = pending_responses_.erase(it); continue; }
        const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
            return item.config.name == it->name;
        });
        bool complete = false;
        if (it->command == control_command::save) {
            complete = save_completed;
        } else if (it->command == control_command::reload) {
            complete = !runtime_state_.reload_pending;
        } else {
            control_operation_type operation_type = control_operation_type::stop;
            if (it->command == control_command::restart) operation_type = control_operation_type::restart;
            else if (it->command == control_command::remove) operation_type = control_operation_type::remove;
            control_operation_state state;
            state.process_exists = process != processes_.end();
            state.definition_exists = config_map_.find(it->name) != config_map_.end();
            if (state.process_exists) {
                state.process_active = process->has_process ||
                    process->termination.phase() != termination_phase::none;
                state.generation = process->generation;
            }
            complete = control_operation{operation_type, it->generation}.complete(state);
        }
        if (!complete) {
            if (it->command == control_command::restart && it->show_log &&
                process != processes_.end() && !process->has_process &&
                runtime_state_.dependencies.state(it->name) == dependency_runtime_state::failed) {
                session->send(make_control_response(it->request, -1, "ERR failed to restart process\n"));
                session->finish();
                it = pending_responses_.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        const int response_code = it->command == control_command::save && save_result != 0 ? -1 : 0;
        auto response = make_control_response(it->request, response_code,
            it->command == control_command::save ?
                (save_result == 0 ? "OK saved\n" : "ERR save failed\n") :
            it->command == control_command::reload ? "OK reloaded\n" :
            it->command == control_command::restart ? "OK restarted\n" :
            it->command == control_command::remove ? "OK deleted\n" : "OK stopped\n");
        if (it->command == control_command::restart && it->show_log && process != processes_.end()) {
            program_log_response metadata;
            metadata.mode = log_request_mode::live;
            metadata.generation = process->generation;
            metadata.last_pid = process->last_pid;
            append_program_log_response(response.payload, metadata);
        }
        session->send(std::move(response));
        if (it->command == control_command::restart && it->show_log) {
            begin_log_stream(session, it->request, it->name, false, log_request_mode::live);
        } else {
            session->finish();
        }
        it = pending_responses_.erase(it);
    }
}

void ControlServer::begin_log_stream(const std::shared_ptr<AsyncNamedPipeSession> &session,
                                     const protocol_message &request, const std::string &name,
                                     bool follow_process, log_request_mode mode) {
    auto config = config_map_.find(name);
    if (config == config_map_.end()) {
        session->send(make_control_response(request, -1, "ERR process not found\n"));
        session->finish();
        return;
    }
    const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == name;
    });
    if (process == processes_.end()) {
        session->send(make_control_response(request, -1, "ERR process not found\n"));
        session->finish();
        return;
    }
    if (mode == log_request_mode::live && !process->has_process && !follow_process) {
        session->send(make_control_response(request, -1, "ERR `" + name +
            "` is not running; use `pm log " + name + " --history` to show the last completed generation\n"));
        session->finish();
        return;
    }
    if (mode == log_request_mode::history && process->has_process) {
        session->send(make_control_response(request, -1, "ERR `" + name +
            "` is running; use `pm log " + name + "`\n"));
        session->finish();
        return;
    }
    if (mode == log_request_mode::history &&
        (process->generation == 0 || !process->has_last_exit || process->last_exit_time_unix_ms <= 0)) {
        session->send(make_control_response(request, -1,
            "ERR no completed log generation available for `" + name + "`\n"));
        session->finish();
        return;
    }
    if (request.type == static_cast<std::uint16_t>(control_command::log) &&
        (mode == log_request_mode::history || process->has_process)) {
        auto response = make_control_response(request, 0, "OK");
        program_log_response metadata;
        metadata.mode = mode;
        metadata.generation = process->generation;
        metadata.last_pid = process->last_pid;
        metadata.last_exit_time_unix_ms = process->last_exit_time_unix_ms;
        if (process->has_last_exit) {
            metadata.exit_reason = "exited";
            metadata.exit_code = static_cast<std::int32_t>(process->last_exit_code);
        }
        append_program_log_response(response.payload, metadata);
        session->send(std::move(response));
    }
    const bool waiting_for_generation = follow_process && !process->has_process &&
        request.type == static_cast<std::uint16_t>(control_command::log);
    const auto generation = process->generation;
    const auto offset = !waiting_for_generation && generation != 0
        ? process->log_tail.begin_offset() : 0;
    log_streams_.push_back({session, request, name, generation, offset, follow_process,
                            waiting_for_generation, mode});
}

void ControlServer::poll_log_streams() {
    for (auto it = log_streams_.begin(); it != log_streams_.end();) {
        auto session = it->session.lock();
        if (!session) { it = log_streams_.erase(it); continue; }
        const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
            return item.config.name == it->name;
        });
        if (it->waiting_for_generation) {
            if (process != processes_.end() && process->generation != it->generation) {
                if (it->request.type == static_cast<std::uint16_t>(control_command::log)) {
                    auto response = make_control_response(it->request, 0, "OK");
                    program_log_response metadata;
                    metadata.mode = log_request_mode::live;
                    metadata.generation = process->generation;
                    metadata.last_pid = process->last_pid;
                    append_program_log_response(response.payload, metadata);
                    session->send(std::move(response));
                }
                it->generation = process->generation;
                it->offset = process->log_tail.begin_offset();
                it->waiting_for_generation = false;
            } else if (it->request.type == static_cast<std::uint16_t>(control_command::log) &&
                       (process == processes_.end() || !process->restart_pending)) {
                session->send(make_control_response(it->request, -1,
                    "ERR log wait ended before the next generation started\n"));
                session->finish();
                it = log_streams_.erase(it);
                continue;
            } else { ++it; continue; }
        }
        const bool generation_exists = process != processes_.end() && process->generation == it->generation;
        const bool generation_active = generation_exists && process->has_process;
        const auto tail_begin = generation_exists ? process->log_tail.begin_offset() : 0;
        if (generation_exists && it->offset < tail_begin) it->offset = tail_begin;
        if (generation_exists && it->offset < process->log_tail.total_bytes()) {
            const auto content = process->log_tail.read(it->offset, 16 * 1024);
            const auto count = content.size();
            protocol_message chunk;
            chunk.type = it->request.type;
            chunk.request_id = it->request.request_id;
            chunk.flags = static_cast<std::uint8_t>(protocol_flag_response | protocol_flag_stream |
                protocol_flag_more);
            fappend_value<std::int32_t>(chunk.payload, 1);
            fappend_value(chunk.payload, content);
            session->send(std::move(chunk));
            it->offset += count;
            ++it;
            continue;
        }
        if (generation_active && it->mode == log_request_mode::live) { ++it; continue; }
        {
            protocol_message final_chunk;
            final_chunk.type = it->request.type;
            final_chunk.request_id = it->request.request_id;
            final_chunk.flags = static_cast<std::uint8_t>(protocol_flag_response | protocol_flag_stream);
            fappend_value<std::int32_t>(final_chunk.payload, 0);
            std::string exit_event;
            if (it->mode == log_request_mode::live && generation_exists && process->has_last_exit) {
                exit_event = format_program_exit_event(process->config.name,
                                                        static_cast<std::int64_t>(process->last_pid),
                                                        "exited", process->last_exit_code);
            }
            fappend_value(final_chunk.payload, exit_event);
            session->send(std::move(final_chunk));
        }
        session->finish();
        it = log_streams_.erase(it);
    }
}

pm_tiny::protocol_message ControlServer::handle_message(const pm_tiny::protocol_message &request) {
    const auto decoded = decode_control_request(request);
    if (!decoded.success) return make_control_response(request, -1, "ERR " + decoded.error + "\n");
    if (decoded.command == control_command::list) return handle_list(request);
    if (decoded.command == control_command::info) return handle_info(request);
    const auto &args = decoded.name;
    std::string result;
    switch (decoded.command) {
        case control_command::stop: result = handle_stop(args); break;
        case control_command::start:
            try { return handle_start(request, pm_tiny::read_start_request(request.payload)); }
            catch (const std::exception &error) {
                return make_control_response(request, -1, std::string("ERR ") + error.what() + "\n");
            }
        case control_command::restart: result = handle_restart(args); break;
        case control_command::save: result = handle_save(); break;
        case control_command::remove: result = handle_delete(args); break;
        case control_command::version: {
            protocol_message response;
            response.type = request.type;
            response.request_id = request.request_id;
            response.flags = protocol_flag_response;
            fappend_value<std::int32_t>(response.payload, 0);
            fappend_value(response.payload, std::string("success"));
            fappend_value(response.payload, std::string(PM_TINY_VERSION));
            return response;
        }
        case control_command::log: result = handle_log(args); break;
        case control_command::inspect: return handle_inspect(request, args);
        case control_command::reload: result = handle_reload(); break;
        case control_command::app_ready: result = handle_app_signal(args, true); break;
        case control_command::app_tick: result = handle_app_signal(args, false); break;
        case control_command::quit: {
            should_stop_.store(true);
            running_.store(false);
            protocol_message response;
            response.type = request.type;
            response.request_id = request.request_id;
            response.flags = protocol_flag_response;
            fappend_value<std::int32_t>(response.payload, 0);
            fappend_value(response.payload, std::string("success"));
            fappend_value<std::int32_t>(response.payload, static_cast<std::int32_t>(GetCurrentProcessId()));
            return response;
        }
        case control_command::list: break;
        case control_command::info: break;
    }
    const bool error = result.rfind("ERR", 0) == 0;
    return make_control_response(request, error ? -1 : 0, result);
}

pm_tiny::protocol_message ControlServer::handle_info(const pm_tiny::protocol_message &request) {
    const auto now_ms = monotonic_millis();
    auto snapshot = make_daemon_info_base(
        daemon_configuration_, daemon_options_, daemon_info_platform::windows_os, PM_TINY_VERSION,
        static_cast<std::int64_t>(GetCurrentProcessId()),
        static_cast<std::int64_t>(now_ms >= started_ms_ ? now_ms - started_ms_ : 0));
    snapshot.state = should_stop_.load() ? daemon_runtime_state::stopping :
                     runtime_state_.reload_pending ? daemon_runtime_state::reloading :
                     daemon_runtime_state::running;
    snapshot.persistence_active = persistence_.busy();
    for (const auto &process : processes_) {
        if (process.config_source == "runtime") ++snapshot.runtime_definition_count;
        else ++snapshot.file_config_count;
    }
    snapshot.requested_process_tree_mode = "job_object";
    snapshot.effective_process_tree_mode = "job_object";
    snapshot.sources["requested_process_tree_mode"] = daemon_config_source::derived;
    snapshot.sources["cgroup_root"] = daemon_config_source::derived;
    snapshot.cgroup_root.clear();
    const auto log = daemon_log_snapshot();
    snapshot.log_level = log.level;
    snapshot.log_max_size_kb = static_cast<std::int32_t>(log.max_size_bytes / 1024U);
    snapshot.log_archive_count = log.archive_count;
    snapshot.log_console_mirror = log.mirror_console;
    snapshot.log_sink = log.sink == "file" ? daemon_log_sink::file :
        log.sink == "console_fallback" ? daemon_log_sink::console_fallback : daemon_log_sink::console;
    snapshot.log_degraded = log.degraded; snapshot.log_last_error = log.last_error;
    snapshot.dynamic_create = true; snapshot.pty = false; snapshot.switch_user = false;
    snapshot.oom_adjust = false; snapshot.failure_action = false; snapshot.service_mode = true;
    snapshot.process_tree_backends = {"job_object"};
    auto response = make_control_response(request, 0, "success");
    append_daemon_info(response.payload, snapshot);
    return response;
}

pm_tiny::protocol_message ControlServer::handle_list(const pm_tiny::protocol_message &request) {
    pm_tiny::protocol_message response;
    response.type = request.type;
    response.request_id = request.request_id;
    response.flags = pm_tiny::protocol_flag_response;
    pm_tiny::fappend_value<std::int32_t>(response.payload, 0);
    pm_tiny::fappend_value(response.payload, "success");
    std::vector<pm_tiny::process_list_entry> entries;
    entries.reserve(processes_.size());
    const auto now_ms = monotonic_millis();
    for (const auto &proc : processes_) {
        entries.push_back(make_process_list_entry(
            static_cast<const prog_cfg_t &>(proc.config), snapshot_runtime(proc, runtime_state_, now_ms)));
    }
    pm_tiny::append_process_list(response.payload, entries);
    return response;
}

std::string ControlServer::handle_stop(const std::string &name) {
    auto it = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &proc) {
        return proc.config.name == name;
    });
    if (it == processes_.end()) {
        return "ERR process not found\n";
    }
    ProcessHandle &proc = *it;
    if (!proc.has_process || proc.proc_info.hProcess == nullptr) {
        apply_manual_stop(proc, runtime_state_);
        return "OK already stopped\n";
    }
    std::string terminate_error;
    if (!request_program_termination(proc, CompletionAction::remove, 0, terminate_error)) {
        return "ERR " + terminate_error + "\n";
    }
    apply_manual_stop(proc, runtime_state_);
    if (!terminate_error.empty()) daemon_log_message(daemon_log_level_t::warn, proc.config.name + ": " + terminate_error);
    const auto generation = proc.generation;
    (void)generation;
    return "OK stop scheduled\n";
}

std::string ControlServer::handle_restart(const std::string &name) {
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
    const auto generation = it->generation;
    std::string terminate_error;
    if (!request_program_termination(*it, CompletionAction::restart, 0, terminate_error)) {
        return "ERR " + terminate_error + "\n";
    }
    if (!terminate_error.empty()) daemon_log_message(daemon_log_level_t::warn, it->config.name + ": " + terminate_error);
    (void)generation;
    return "OK restart scheduled\n";
}

pm_tiny::protocol_message ControlServer::handle_start(const pm_tiny::protocol_message &message,
                                                       const pm_tiny::start_request &request) {
    const auto fail = [&](const std::string &text) {
        return make_control_response(message, -1, "ERR " + text + "\n");
    };
    if (runtime_state_.reload_pending || should_stop_.load()) return fail("daemon is busy");
    auto cfg_iter = config_map_.find(request.name);
    bool has_config = cfg_iter != config_map_.end();
    if (request.mode == start_mode::existing && !has_config) return fail("process not found");
    if (request.mode == start_mode::create && has_config) return fail("process already exists");
    ProgramConfig cfg = request.mode == start_mode::existing ? cfg_iter->second : ProgramConfig{};
    if (request.mode == start_mode::create) static_cast<prog_cfg_t &>(cfg) = request.config;
    cfg.name = request.name;
    if (cfg.executable.empty()) return fail("missing executable");
    if (!cfg.run_as.empty() || cfg.oom_score_adj != 0 || cfg.pty || cfg.failure_action != failure_action_t::SKIP)
        return fail("unsupported Windows start option");
    if (request.mode == start_mode::create) cfg.envs = request.inherited_env;
    const auto current_process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == cfg.name;
    });
    if (request.mode == start_mode::existing && current_process != processes_.end() && current_process->has_process)
        return fail("process already running");
    if (request.mode == start_mode::existing && current_process != processes_.end()) {
        current_process->restart_state.reset();
        current_process->restart_pending = false;
        current_process->restart_due_ms = 0;
        runtime_state_.dependencies.mark_idle(cfg.name);
    }
    if (cfg.cwd.empty()) {
        cfg.cwd = ".";
    }
    std::vector<ProgramConfig> configs;
    configs.reserve(config_map_.size() + 1);
    for (const auto &entry : config_map_) if (entry.first != cfg.name) configs.push_back(entry.second);
    configs.push_back(cfg);
    std::string dependency_error;
    if (!rebuild_dependencies(runtime_state_, configs, dependency_error)) return fail(dependency_error);
    for (const auto &process : processes_) {
        if (process.has_process && (process.ready || process.config.start_timeout == 0))
            runtime_state_.dependencies.mark_ready(process.config.name);
    }
    config_map_[cfg.name] = cfg;
    ensure_process_records(processes_, configs);
    if (request.mode == start_mode::create) {
        const auto created = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
            return item.config.name == cfg.name;
        });
        if (created != processes_.end()) created->config_source = "runtime";
    }
    const auto failures = schedule_dependency_launch(
        processes_, runtime_state_, runtime_state_.dependencies.request_closure(cfg.name));
    if (!failures.empty()) {
        if (request.mode == start_mode::create) {
            config_map_.erase(cfg.name);
            const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
                return item.config.name == cfg.name;
            });
            if (process != processes_.end() && !process->has_process) processes_.erase(process);
            std::vector<ProgramConfig> restored;
            restored.reserve(config_map_.size());
            for (const auto &entry : config_map_) restored.push_back(entry.second);
            std::string ignored;
            rebuild_dependencies(runtime_state_, restored, ignored);
            for (const auto &item : processes_) {
                if (item.has_process && (item.ready || item.config.start_timeout == 0))
                    runtime_state_.dependencies.mark_ready(item.config.name);
            }
        }
        return fail("failed to start: " + failures.front());
    }
    const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &item) {
        return item.config.name == cfg.name;
    });
    pm_tiny::start_response start;
    start.state = "waiting";
    if (process != processes_.end() && process->has_process) {
        start.result = start_result::started;
        start.pid = static_cast<std::int64_t>(process->proc_info.dwProcessId);
        start.state = process->ready || process->config.start_timeout == 0 ? "online" : "starting";
    } else if (runtime_state_.dependencies.state(cfg.name) == dependency_runtime_state::blocked) {
        start.result = start_result::blocked;
        start.state = "blocked";
        start.blocked_by = runtime_state_.dependencies.blocked_by(cfg.name);
    } else {
        start.result = start_result::waiting;
        start.blocked_by = runtime_state_.dependencies.waiting_for(cfg.name);
    }
    pm_tiny::protocol_message response;
    response.type = message.type;
    response.request_id = message.request_id;
    response.flags = pm_tiny::protocol_flag_response;
    pm_tiny::fappend_value<std::int32_t>(response.payload, 0);
    pm_tiny::fappend_value(response.payload,
        start.result == start_result::started ? std::string("started") : start.state);
    pm_tiny::append_start_response(response.payload, start);
    return response;
}

std::string ControlServer::handle_delete(const std::string &name) {
    delete_plan deletion;
    std::string error;
    if (!prepare_delete(name, config_map_, runtime_state_, deletion, error)) return "ERR " + error + "\n";
    auto it = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &proc) {
        return proc.config.name == name;
    });
    if (it != processes_.end()) {
        it->disable_restart = true;
        if (it->has_process && it->proc_info.hProcess != nullptr) {
            std::string terminate_error;
            if (!request_program_termination(*it, CompletionAction::delete_config, 0, terminate_error)) {
                return "ERR " + terminate_error + "\n";
            }
            if (!terminate_error.empty()) daemon_log_message(daemon_log_level_t::warn, it->config.name + ": " + terminate_error);
            commit_delete(name, config_map_, runtime_state_, std::move(deletion));
            return "OK delete scheduled\n";
        } else {
            it->reset();
            processes_.erase(it);
        }
    }
    commit_delete(name, config_map_, runtime_state_, std::move(deletion));
    return "OK deleted\n";
}

pm_tiny::protocol_message ControlServer::handle_inspect(const pm_tiny::protocol_message &request,
                                                        const std::string &name) {
    auto it = config_map_.find(name);
    if (it == config_map_.end()) return make_control_response(request, -1, "ERR process not found\n");
    const auto &cfg = it->second;
    const auto process = std::find_if(processes_.begin(), processes_.end(), [&](const ProcessHandle &candidate) {
        return candidate.config.name == name;
    });
    const auto now_ms = monotonic_millis();
    inspect_snapshot snapshot;
    snapshot.config = static_cast<const prog_cfg_t &>(cfg);
    if (process != processes_.end()) {
        snapshot.runtime = snapshot_runtime(*process, runtime_state_, now_ms);
    } else {
        snapshot.runtime.pty = pty_mode_t::unsupported;
        snapshot.runtime.process_tree_backend = "job_object";
        snapshot.runtime.config_source = "file";
    }
    protocol_message response;
    response.type = request.type;
    response.request_id = request.request_id;
    response.flags = protocol_flag_response;
    fappend_value<std::int32_t>(response.payload, 0);
    fappend_value(response.payload, "success");
    append_inspect_snapshot(response.payload, snapshot);
    return response;
}

std::string ControlServer::handle_log(const std::string &name) {
    auto it = config_map_.find(name);
    if (it == config_map_.end()) return "ERR process not found\n";
    const auto &cfg = it->second;
    const auto paths = derive_log_paths(cfg.log_dir.empty() ? app_log_dir_ : cfg.log_dir,
                                        cfg.name, cfg.log_mode, cfg.log_file_name);
    const auto path = paths.front();
    std::ifstream input(path, std::ios::binary);
    if (!input) return "ERR log file not found\n";
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string ControlServer::handle_save() {
    return "ERR asynchronous save required\n";
}

std::string ControlServer::handle_reload() {
    if (persistence_.busy()) return "ERR persistence operation busy\n";
    auto loaded = load_program_configs(program_config_path_, app_environ_dir_);
    if (!loaded.error_message.empty()) return "ERR " + loaded.error_message + "\n";
    if (runtime_state_.reload_pending || should_stop_.load()) return "ERR daemon is busy\n";
    runtime_state_.reload_pending = true;
    runtime_state_.reload_programs = std::move(loaded.programs);
    for (auto iter = processes_.rbegin(); iter != processes_.rend(); ++iter) {
        iter->disable_restart = true;
        std::string error;
        if (!request_program_termination(*iter, CompletionAction::remove, 0, error)) {
            daemon_log_message(daemon_log_level_t::error, "Failed to stop `" + iter->config.name + "` for reload: " + error);
        } else if (!error.empty()) {
            daemon_log_message(daemon_log_level_t::warn, iter->config.name + ": " + error);
        }
    }
    return "OK reload scheduled\n";
}

std::string ControlServer::handle_app_signal(const std::string &name, bool ready_signal) {
    if (name.empty()) return "ERR missing process name\n";
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
