//
// Created by qianlinluo@foxmail.com on 23-7-27.
//
#include "pm_tiny_funcs.h"
#include "pm_tiny_server.h"
#include "prog.h"
#include <cassert>
#include "daemon_log.h"
#include "process_list.h"
#include "runtime_snapshot.h"
#include "time_util.h"
#include "control_command.h"
#include "prog_cfg_order.h"
#include "daemon_info.h"
#include "launch_environment.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>

std::string msg_cmd_not_completed(const std::string &name) {
    std::string msg = "On target `";
    msg += name;
    msg += "`, another operation is not completed, try later.";
    return msg;
}

std::string msg_DAG_not_completed(const std::string &name) {
    std::string msg = "On target `";
    msg += name;
    msg += "`, DAG start of dependencies did not complete, try again later.";
    return msg;
}

std::string msg_server_stoping() {
    return "PM_Tiny is stopping, the operation cannot be performed";
}

namespace {

bool heartbeat_peer_matches(pm_tiny::pm_tiny_server_t &server,
                            const pm_tiny::session_ptr_t &session,
                            const pm_tiny::prog_ptr_t &prog,
                            const char *operation) {
    const pid_t peer_pid = session && session->has_peer_credentials() ? session->peer_pid() : -1;
    const bool matches = prog && prog->instance.pid > 0 && prog->instance.tree.active &&
                         server.process_tree && peer_pid > 0 &&
                         server.process_tree->contains(prog->instance.tree, peer_pid);
    if (!matches) {
        PM_TINY_DLOG_ERROR("reject `%s` %s from pid=%d uid=%u gid=%u: peer is not in current process tree",
                           prog ? prog->name.c_str() : "<unknown>", operation,
                           static_cast<int>(peer_pid),
                           session && session->has_peer_credentials()
                               ? static_cast<unsigned>(session->peer_uid()) : 0U,
                           session && session->has_peer_credentials()
                               ? static_cast<unsigned>(session->peer_gid()) : 0U);
        if (session) session->close();
    }
    return matches;
}

pm_tiny::prog_cfg_t snapshot_config(const pm_tiny::prog_info_t &prog) {
    pm_tiny::prog_cfg_t config;
    config.name = prog.name;
    config.cwd = prog.work_dir;
    config.executable = prog.executable;
    config.args = prog.args;
    config.kill_timeout_s = prog.kill_timeout_sec;
    config.run_as = prog.run_as;
    config.env_vars = prog.env_vars;
    config.depends_on = prog.depends_on;
    config.start_timeout = prog.start_timeout;
    config.failure_action = prog.failure_action;
    config.daemon = prog.daemon;
    config.heartbeat_timeout = prog.heartbeat_timeout;
    config.oom_score_adj = prog.oom_score_adj;
    config.pty = prog.use_pty;
    config.log_mode = prog.log_mode;
    config.log_dir = prog.log_dir;
    config.log_file_name = prog.log_file_name;
    config.log_max_size_kb = prog.log_max_size_kb;
    config.log_archive_count = prog.log_archive_count;
    config.restart_delay_ms = prog.restart_config.delay_ms;
    config.restart_max_delay_ms = prog.restart_config.max_delay_ms;
    config.restart_window_ms = prog.restart_config.window_ms;
    config.restart_max_attempts = prog.restart_config.max_attempts;
    config.restart_reset_after_ms = prog.restart_config.reset_after_ms;
    return config;
}

pm_tiny::runtime_snapshot snapshot_runtime(const pm_tiny::prog_info_t &prog, std::int64_t now_ms) {
    pm_tiny::runtime_snapshot runtime;
    runtime.pid = prog.instance.pid;
    runtime.generation = prog.instance.generation;
    runtime.state = prog.state;
    runtime.restart_count = prog.dead_count;
    runtime.ready = prog.state == PM_TINY_PROG_STATE_RUNING;
    runtime.heartbeat_enabled = prog.heartbeat_timeout > 0;
    runtime.has_last_tick_age = runtime.heartbeat_enabled && prog.last_tick_timepoint > 0;
    if (runtime.has_last_tick_age)
        runtime.last_tick_age_ms = std::max<std::int64_t>(0, now_ms - prog.last_tick_timepoint);
    runtime.has_uptime = prog.instance.pid > 0 && prog.last_startup_ms > 0;
    if (runtime.has_uptime) runtime.uptime_ms = std::max<std::int64_t>(0, now_ms - prog.last_startup_ms);
    if (prog.instance.pid > 0) {
        runtime.rss_kib = pm_tiny::get_vm_rss_kib(prog.instance.pid);
        runtime.has_rss = runtime.rss_kib >= 0;
    }
    runtime.pty = prog.use_pty ? pm_tiny::pty_mode_t::enabled : pm_tiny::pty_mode_t::disabled;
    runtime.restart_pending = prog.restart_pending;
    if (runtime.restart_pending)
        runtime.restart_delay_remaining_ms = std::max<std::int64_t>(0, prog.restart_due_ms - now_ms);
    runtime.restart_attempts_in_window = static_cast<std::int32_t>(prog.restart_state.attempts_ms.size());
    runtime.restart_suppressed = prog.restart_state.suppressed;
    if (runtime.restart_suppressed) runtime.restart_suppression_reason = pm_tiny::restart_attempt_limit_reason;
    runtime.has_last_exit = prog.has_last_exit;
    if (runtime.has_last_exit) {
        if (WIFEXITED(prog.last_wstatus)) {
            runtime.last_exit_reason = pm_tiny::exit_reason_t::exited;
            runtime.last_exit_code = WEXITSTATUS(prog.last_wstatus);
        } else if (WIFSIGNALED(prog.last_wstatus)) {
            runtime.last_exit_reason = pm_tiny::exit_reason_t::signaled;
            runtime.last_exit_code = WTERMSIG(prog.last_wstatus);
        } else {
            runtime.last_exit_reason = pm_tiny::exit_reason_t::unknown;
            runtime.last_exit_code = prog.last_wstatus;
        }
    }
    runtime.process_tree_backend = prog.process_tree_backend;
    runtime.process_tree_degraded = prog.process_tree_degraded ||
        (prog.tree_controller && prog.tree_controller->degraded());
    runtime.process_tree_degradation_reason = !prog.process_tree_degradation_reason.empty() ?
        prog.process_tree_degradation_reason :
        (prog.tree_controller ? prog.tree_controller->degradation_reason() : std::string());
    runtime.config_source = prog.config_source;
    runtime.log_degraded = prog.log_health.degraded;
    runtime.log_dropped_bytes = prog.log_health.dropped_bytes;
    runtime.log_last_error = prog.log_health.last_error;
    runtime.log_retry_remaining_ms = prog.log_health.degraded
        ? std::max<std::int64_t>(0, prog.log_health.retry_due_ms - now_ms) : 0;
    for (const auto &path : prog.logfile) if (!path.empty()) runtime.log_paths.push_back(path);
    return runtime;
}

} // namespace

pm_tiny::frame_ptr_t make_server_reloading_frame() {
    auto wf = std::make_unique<pm_tiny::frame_t>();
    pm_tiny::fappend_value<int>(*wf, -1);
    std::string msg = "PM_Tiny is reloading, the operation cannot be performed";
    pm_tiny::fappend_value(*wf, msg);
    return wf;
}

pm_tiny::frame_ptr_t make_server_stoping_frame() {
    auto wf = std::make_unique<pm_tiny::frame_t>();
    pm_tiny::fappend_value<int>(*wf, -1);
    std::string msg = msg_server_stoping();
    pm_tiny::fappend_value(*wf, msg);
    return wf;
}

pm_tiny::frame_ptr_t make_daemon_info_data(pm_tiny::pm_tiny_server_t &server) {
    const auto now_ms = pm_tiny::time::gettime_monotonic_ms();
#ifdef __ANDROID__
    const auto platform = pm_tiny::daemon_info_platform::android_os;
#else
    const auto platform = pm_tiny::daemon_info_platform::linux_os;
#endif
    auto snapshot = pm_tiny::make_daemon_info_base(
        server.effective_daemon_config, server.daemon_options, platform, PM_TINY_VERSION,
        static_cast<std::int64_t>(getpid()), std::max<std::int64_t>(0, now_ms - server.started_monotonic_ms));
    snapshot.state = server.is_reloading() ? pm_tiny::daemon_runtime_state::reloading :
                     server.is_exiting() ? pm_tiny::daemon_runtime_state::stopping :
                     pm_tiny::daemon_runtime_state::running;
    snapshot.persistence_active = server.persistence_busy();
    for (const auto &prog : server.pm_tiny_progs) {
        if (prog->config_source == "runtime") ++snapshot.runtime_definition_count;
        else ++snapshot.file_config_count;
    }
    snapshot.effective_process_tree_mode = server.process_tree
        ? pm_tiny::process_tree_mode_name(server.process_tree->effective_mode()) : "unavailable";
    if (server.process_tree) {
        snapshot.cgroup_root = server.process_tree->root();
        snapshot.sources["cgroup_root"] = pm_tiny::daemon_config_source::derived;
    }
    snapshot.subreaper_enabled = server.subreaper_enabled;
    snapshot.process_tree_degraded = !server.subreaper_enabled ||
        (server.process_tree && server.process_tree->degraded());
    if (!server.subreaper_error.empty()) snapshot.process_tree_degradation_reason = server.subreaper_error;
    if (server.process_tree && !server.process_tree->degradation_reason().empty()) {
        if (!snapshot.process_tree_degradation_reason.empty()) snapshot.process_tree_degradation_reason += "; ";
        snapshot.process_tree_degradation_reason += server.process_tree->degradation_reason();
    }
    const auto log = pm_tiny::daemon_log_snapshot();
    snapshot.log_level = log.level;
    snapshot.log_max_size_kb = static_cast<std::int32_t>(log.max_size_bytes / 1024U);
    snapshot.log_archive_count = log.archive_count;
    snapshot.log_console_mirror = log.mirror_console;
    snapshot.log_sink = log.sink == "file" ? pm_tiny::daemon_log_sink::file :
        log.sink == "console_fallback" ? pm_tiny::daemon_log_sink::console_fallback :
        pm_tiny::daemon_log_sink::console;
    snapshot.log_degraded = log.degraded; snapshot.log_last_error = log.last_error;
    snapshot.pty = true; snapshot.switch_user = true; snapshot.oom_adjust = true;
    snapshot.failure_action = true; snapshot.service_mode = false;
    snapshot.process_tree_backends = {"cgroup_v2", "process_group"};
    auto frame = pm_tiny::make_control_response_payload(0, "success");
    pm_tiny::append_daemon_info(*frame, snapshot);
    return frame;
}

pm_tiny::frame_ptr_t make_prog_info_data(pm_tiny::proglist_t &pm_tiny_progs) {
    auto f = std::make_unique<pm_tiny::frame_t>();
    pm_tiny::fappend_value<std::int32_t>(*f, 0);
    pm_tiny::fappend_value(*f, "success");
    std::vector<pm_tiny::process_list_entry> entries;
    entries.reserve(pm_tiny_progs.size());
    const auto now_ms = pm_tiny::time::gettime_monotonic_ms();
    for (auto &prog_info: pm_tiny_progs) {
        entries.push_back(pm_tiny::make_process_list_entry(
            snapshot_config(*prog_info), snapshot_runtime(*prog_info, now_ms)));
    }
    pm_tiny::append_process_list(*f, entries);
    return f;
}

std::unique_ptr<pm_tiny::frame_t> handle_cmd_start(pm_tiny::pm_tiny_server_t &pm_tiny_server,
                                                   pm_tiny::iframe_stream &ifs,
                                                   std::shared_ptr<pm_tiny::session_t> &session) {
    using pm_tiny::prog_ptr_t;
    auto wf = std::make_unique<pm_tiny::frame_t>();
    const auto fail = [&](int code, const std::string &message) {
        pm_tiny::fappend_value<int>(*wf, code);
        pm_tiny::fappend_value(*wf, message);
    };
    pm_tiny::start_request request;
    try { request = pm_tiny::read_start_request(ifs.remaining_frame()); }
    catch (const std::exception &error) { fail(-1, error.what()); return wf; }
    auto &progs = pm_tiny_server.pm_tiny_progs;
    auto iter = std::find_if(progs.begin(), progs.end(), [&](prog_ptr_t prog) { return prog->name == request.name; });
    prog_ptr_t target = iter == progs.end() ? nullptr : *iter;
    std::unique_ptr<pm_tiny::prog_info_t> created;
    if (request.mode == pm_tiny::start_mode::existing) {
        if (!target) { fail(-2, "process not found: `" + request.name + "`"); return wf; }
        pm_tiny::passwd_t target_user;
        const bool target_resolved = target->run_as.empty()
            ? pm_tiny::get_user_from_uid(geteuid(), target_user) == 0
            : pm_tiny::get_uid_from_username(target->run_as.c_str(), target_user) == 0;
        PM_TINY_DLOG_INFO("start request peer_pid=%d peer_uid=%u peer_gid=%u name=%s "
                           "target_user=%s target_uid=%lld target_gid=%lld executable=%s identity=config",
                           session && session->has_peer_credentials() ? static_cast<int>(session->peer_pid()) : -1,
                           session && session->has_peer_credentials() ? static_cast<unsigned>(session->peer_uid()) : 0U,
                           session && session->has_peer_credentials() ? static_cast<unsigned>(session->peer_gid()) : 0U,
                           request.name.c_str(),
                           target->run_as.empty() ? "<daemon>" : target->run_as.c_str(),
                           target_resolved ? static_cast<long long>(target_user.pw_uid) : -1LL,
                           target_resolved ? static_cast<long long>(target_user.pw_gid) : -1LL,
                           target->executable.c_str());
    } else {
        if (target) { fail(-2, "process already exists: `" + request.name + "`"); return wf; }
        auto cfg = request.config;
        if (cfg.name != request.name || cfg.executable.empty() || cfg.cwd.empty()) {
            fail(-3, "invalid runtime definition"); return wf;
        }
        if (!session || !session->has_peer_credentials()) {
            fail(-3, "cannot determine start request peer identity");
            return wf;
        }
        pm_tiny::passwd_t peer_user;
        if (pm_tiny::get_user_from_uid(session->peer_uid(), peer_user) == -1) {
            const int error = errno;
            fail(-3, "cannot resolve peer uid " + std::to_string(session->peer_uid()) +
                     ": " + std::strerror(error));
            return wf;
        }
        const bool explicit_user = !cfg.run_as.empty();
        pm_tiny::passwd_t target_user;
        if (explicit_user) {
            if (pm_tiny::get_uid_from_username(cfg.run_as.c_str(), target_user) == -1) {
                const int error = errno;
                fail(-3, "cannot resolve target user `" + cfg.run_as + "`: " + std::strerror(error));
                return wf;
            }
        } else {
            target_user = peer_user;
            cfg.run_as = peer_user.pw_name;
        }
        const bool cross_user = target_user.pw_uid != peer_user.pw_uid ||
                                target_user.pw_gid != peer_user.pw_gid;
        if (geteuid() != 0 &&
            (target_user.pw_uid != geteuid() || target_user.pw_gid != getegid())) {
            PM_TINY_DLOG_ERROR("reject start peer_pid=%d peer_uid=%u name=%s target_user=%s "
                                "target_uid=%u: daemon uid=%u cannot switch identity",
                                static_cast<int>(session->peer_pid()),
                                static_cast<unsigned>(session->peer_uid()), request.name.c_str(),
                                cfg.run_as.c_str(), static_cast<unsigned>(target_user.pw_uid),
                                static_cast<unsigned>(geteuid()));
            fail(-3, "pm_tiny uid " + std::to_string(geteuid()) + " cannot start `" +
                     request.name + "` as user `" + cfg.run_as + "`");
            return wf;
        }
        if (cross_user && !pm_tiny::executable_has_path(cfg.executable)) {
            PM_TINY_DLOG_ERROR("reject cross-user start peer_pid=%d peer_uid=%u name=%s "
                                "target_user=%s executable=%s: executable has no path",
                                static_cast<int>(session->peer_pid()),
                                static_cast<unsigned>(session->peer_uid()), request.name.c_str(),
                                cfg.run_as.c_str(), cfg.executable.c_str());
            fail(-3, "cross-user start requires executable containing `/`: `" + cfg.executable + "`");
            return wf;
        }
        if (cross_user) {
            request.inherited_env = pm_tiny::compose_launch_environment(
                request.inherited_env, {}, &target_user, true);
        }
        PM_TINY_DLOG_INFO("start request peer_pid=%d peer_uid=%u peer_gid=%u name=%s "
                           "target_user=%s target_uid=%u target_gid=%u executable=%s identity=%s",
                           static_cast<int>(session->peer_pid()),
                           static_cast<unsigned>(session->peer_uid()),
                           static_cast<unsigned>(session->peer_gid()), request.name.c_str(),
                           cfg.run_as.c_str(), static_cast<unsigned>(target_user.pw_uid),
                           static_cast<unsigned>(target_user.pw_gid), cfg.executable.c_str(),
                           explicit_user ? "explicit" : "peer-default");
        created = pm_tiny_server.create_prog(cfg, request.inherited_env);
        if (!created) { fail(-3, "cannot create `" + request.name + "`"); return wf; }
        target = created.get();
        target->depends_on = cfg.depends_on;
        target->start_timeout = cfg.start_timeout;
        target->failure_action = cfg.failure_action;
        target->daemon = cfg.daemon;
        target->heartbeat_timeout = cfg.heartbeat_timeout;
        target->oom_score_adj = cfg.oom_score_adj;
        target->env_vars = cfg.env_vars;
        target->restart_config.delay_ms = cfg.restart_delay_ms;
        target->restart_config.max_delay_ms = cfg.restart_max_delay_ms;
        target->restart_config.window_ms = cfg.restart_window_ms;
        target->restart_config.max_attempts = cfg.restart_max_attempts;
        target->restart_config.reset_after_ms = cfg.restart_reset_after_ms;
        target->config_source = "runtime";
        std::vector<pm_tiny::prog_cfg_t> configs;
        for (const auto existing : progs) {
            pm_tiny::prog_cfg_t item;
            item.name = existing->name;
            item.depends_on = existing->depends_on;
            configs.push_back(std::move(item));
        }
        configs.push_back(cfg);
        std::string dependency_error;
        if (!pm_tiny::validate_and_order_prog_cfgs(configs, dependency_error)) {
            fail(-3, dependency_error); return wf;
        }
        if (pm_tiny_server.start_and_add_prog(target) != 0) {
            const int error = errno == 0 ? EIO : errno;
            fail(-3, "cannot start `" + request.name + "` as user `" + cfg.run_as +
                     "`: " + std::strerror(error));
            return wf;
        }
        created.release();
    }
    if (target->instance.pid == -1 && request.mode == pm_tiny::start_mode::existing) {
        target->reset_restart_policy();
        if (pm_tiny_server.request_start(target) != 0) {
            fail(-1, std::string("start failed: ") + strerror(errno)); return wf;
        }
    } else if (target->instance.pid != -1 && request.mode == pm_tiny::start_mode::existing) {
        fail(-2, "`" + request.name + "` already running"); return wf;
    }
    auto state = pm_tiny_server.dependency_runtime_.state(request.name);
    pm_tiny::start_response response;
    response.pid = target->instance.pid;
    response.state = pm_state_to_str(target->state);
    if (state == pm_tiny::dependency_runtime_state::blocked) {
        response.result = pm_tiny::start_result::blocked;
        response.blocked_by = pm_tiny_server.dependency_runtime_.blocked_by(request.name);
    } else if (target->instance.pid <= 0) {
        response.result = pm_tiny::start_result::waiting;
        response.blocked_by = pm_tiny_server.dependency_runtime_.waiting_for(request.name);
    }
    else response.result = pm_tiny::start_result::started;
    pm_tiny::fappend_value<int>(*wf, 0);
    pm_tiny::fappend_value(*wf, "success");
    pm_tiny::append_start_response(*wf, response);
    if (request.show_log && response.result != pm_tiny::start_result::blocked) target->add_session(session.get());
    return wf;
}

void handle_cmd_inspect(pm_tiny::pm_tiny_server_t &pm_tiny_server,
                        pm_tiny::iframe_stream &ifs,
                        std::shared_ptr<pm_tiny::session_t> &session) {
    auto &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    std::string name;
    ifs >> name;
    auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                             [&name](const pm_tiny::prog_ptr_t &prog) {
                                 return prog->name == name;
                             });

    if (iter == pm_tiny_progs.end()) {
        auto wf = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*wf, 0x1);
        pm_tiny::fappend_value(*wf, "not found `" + name + "`");
        session->write_frame(wf);
    } else {
        auto prog_info = *iter;
        auto f = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*f, 0);
        pm_tiny::fappend_value(*f, "success");
        pm_tiny::inspect_snapshot snapshot;
        snapshot.config = snapshot_config(*prog_info);
        snapshot.runtime = snapshot_runtime(*prog_info, pm_tiny::time::gettime_monotonic_ms());
        pm_tiny::append_inspect_snapshot(*f, snapshot);
        session->write_frame(f);
    }
}

using prog_ptr_t = pm_tiny::prog_ptr_t;
using proglist_t = pm_tiny::proglist_t;
using pm_tiny_server_t = pm_tiny::pm_tiny_server_t;

bool server_exiting(pm_tiny_server_t &pm_tiny_server,
                    std::shared_ptr<pm_tiny::session_t> &session) {
    if (pm_tiny_server.is_reloading()) {
        auto f = make_server_reloading_frame();
        session->write_frame(f);
        return true;
    }
    if (pm_tiny_server.is_exiting()) {
        auto f = make_server_stoping_frame();
        session->write_frame(f);
        return true;
    }
    return false;
}

void prog_bind_session(pm_tiny::session_ptr_t &session,
                       const prog_ptr_t &prog, pm_tiny::frame_ptr_t &wf) {
    prog->add_session(session.get());
    pm_tiny::fappend_value<int>(*wf, 0);
    pm_tiny::fappend_value(*wf, "success");
    pm_tiny::program_log_response response;
    response.mode = pm_tiny::log_request_mode::live;
    response.generation = prog->instance.generation;
    response.last_pid = prog->instance.last_pid;
    pm_tiny::append_program_log_response(*wf, response);
}

void handle_frame(pm_tiny_server_t &pm_tiny_server,
                  const pm_tiny::protocol_message &request,
                  pm_tiny::session_ptr_t &session) {
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    const auto decoded_request = pm_tiny::decode_control_request(request);
    if (!decoded_request.success) {
        session->write_frame(pm_tiny::make_control_response_payload(-1, decoded_request.error));
        return;
    }
    pm_tiny::iframe_stream ifs(request.payload);
    const auto command = decoded_request.command;
    if (command == pm_tiny::control_command::list) {
        pm_tiny::frame_ptr_t f = make_prog_info_data(pm_tiny_progs);
        session->write_frame(f);
    } else if (command == pm_tiny::control_command::info) {
        session->write_frame(make_daemon_info_data(pm_tiny_server));
    } else if (command == pm_tiny::control_command::stop) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });

        if (iter == pm_tiny_progs.end()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -0x1);
            pm_tiny::fappend_value(*wf, "not found `" + name + "`");
            session->write_frame(wf);
        } else {
            auto prog_ = *iter;
            if (!prog_->kill_pendingtasks.empty()) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                pm_tiny::fappend_value<int>(*wf, -0x3);
                std::string msg = msg_cmd_not_completed(name);
                pm_tiny::fappend_value(*wf, msg);
                session->write_frame(wf);
            } else {
                if (server_exiting(pm_tiny_server, session)) {
                    return;
                }
                prog_->reset_restart_policy();
                prog_->fail_pending_log_sessions("log wait canceled by stop");
                if (prog_->instance.pid != -1) {
                    auto stop_proc_task =
                            [w = std::weak_ptr<pm_tiny::session_t>(session)](
                                    pm_tiny_server_t &pm_tiny_server) {
                                auto session = w.lock();
                                if (!session)return;
                                if (session->is_close()) {
                                    return;
                                }
                                if (server_exiting(pm_tiny_server, session)) {
                                    return;
                                }
                                auto wf = std::make_unique<pm_tiny::frame_t>();
                                pm_tiny::fappend_value<int>(*wf, 0);
                                pm_tiny::fappend_value(*wf, "success");
                                session->write_frame(wf);
                            };
                    prog_->async_kill_prog();
                    prog_->enqueue_after_termination(stop_proc_task);
                } else {
                    prog_->set_state(PM_TINY_PROG_STATE_STOPED);
                    pm_tiny_server.mark_dependency_stopped(prog_);
                    auto wf = std::make_unique<pm_tiny::frame_t>();
                    pm_tiny::fappend_value<int>(*wf, 0);
                    pm_tiny::fappend_value(*wf, "success");
                    session->write_frame(wf);
                }
            }
        }

    } else if (command == pm_tiny::control_command::start) {
        if (server_exiting(pm_tiny_server, session)) {
            return;
        }
        auto wf = handle_cmd_start(pm_tiny_server, ifs, session);
        session->write_frame(wf);
    } else if (command == pm_tiny::control_command::save) {
        if (pm_tiny_server.is_reloading() || pm_tiny_server.persistence_busy() ||
            !pm_tiny_server.begin_save_proc_to_cfg()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -1);
            pm_tiny::fappend_value(*wf, "persistence operation busy");
            session->write_frame(wf);
        } else {
            pm_tiny_server.wait_save_sessions.emplace_back(session);
        }
    } else if (command == pm_tiny::control_command::remove) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });

        if (iter == pm_tiny_progs.end()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, 0x1);
            pm_tiny::fappend_value(*wf, "not found `" + name + "`");
            session->write_frame(wf);
        } else {
            auto prog_ = *iter;
            const auto dependents = pm_tiny_server.dependency_dependents(name);
            if (!dependents.empty()) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                pm_tiny::fappend_value<int>(*wf, -0x3);
                pm_tiny::fappend_value(*wf, "cannot delete `" + name + "`; required by: " +
                                             mgr::utils::join(dependents, ","));
                session->write_frame(wf);
            } else if (!prog_->kill_pendingtasks.empty()) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                pm_tiny::fappend_value<int>(*wf, -0x3);
                std::string msg = msg_cmd_not_completed(name);
                pm_tiny::fappend_value(*wf, msg);
                session->write_frame(wf);
            } else {
                if (server_exiting(pm_tiny_server, session)) {
                    return;
                }
                auto delete_prog_task =
                        [sw = std::weak_ptr<pm_tiny::session_t>(session)](
                                pm_tiny_server_t &pm_tiny_server) {
                            auto session = sw.lock();
                            if (!session)return;
                            if (session->is_close()) {
                                return;
                            }
                            if (server_exiting(pm_tiny_server, session)) {
                                return;
                            }
                            auto wf = std::make_unique<pm_tiny::frame_t>();
                            pm_tiny::fappend_value<int>(*wf, 0);
                            pm_tiny::fappend_value(*wf, "success");
                            session->write_frame(wf);
                        };

                if (prog_->instance.pid != -1) {
                    prog_->async_kill_prog();
                    prog_->state = PM_TINY_PROG_STATE_REQUEST_DELETE;
                    prog_->enqueue_after_termination(delete_prog_task);
                } else {
                    pm_tiny_server.trigger_DAG_traversal_next_node(prog_);
                    delete_prog_task(pm_tiny_server);
                    pm_tiny_server.remove_prog(prog_);
                }
            }
        }

    } else if (command == pm_tiny::control_command::restart) {
        std::string name;
        int show_log;
        ifs >> name;
        ifs >> show_log;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        if (iter == pm_tiny_progs.end()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -0x1);
            pm_tiny::fappend_value(*wf, "not found `" + name + "`");
            session->write_frame(wf);
        } else {
            auto prog_ = *iter;
            prog_->reset_restart_policy();
            if (!prog_->kill_pendingtasks.empty()) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                pm_tiny::fappend_value<int>(*wf, -0x3);
                std::string msg = msg_cmd_not_completed(name);
                pm_tiny::fappend_value(*wf, msg);
                session->write_frame(wf);
            } else {
                bool is_alive = prog_->instance.pid != -1;
                auto start_prog_task =
                        [sw = std::weak_ptr<pm_tiny::session_t>(session),
                                prog_, show_log](pm_tiny_server_t &pm_tiny_server) {
                            auto session = sw.lock();
                            if (!session)return;
                            if (session->is_close()) {
                                return;
                            }
                            if (server_exiting(pm_tiny_server, session)) {
                                return;
                            }
                            auto wf = std::make_unique<pm_tiny::frame_t>();
                            assert(prog_->state != PM_TINY_PROG_STATE_RUNING);
                            int rc = pm_tiny_server.start_prog(prog_);
                            if (rc == -1) {
                                std::string errmsg(strerror(errno));
                                pm_tiny::fappend_value<int>(*wf, -1);
                                pm_tiny::fappend_value(*wf, errmsg);
                            } else {
                                prog_->dead_count++;
                                if (show_log) {
                                    prog_bind_session(session, prog_, wf);
                                } else {
                                    pm_tiny::fappend_value<int>(*wf, 0);
                                    pm_tiny::fappend_value(*wf, "success");
                                }
                            }
                            session->write_frame(wf);
                        };

                if (is_alive) {
                    prog_->async_kill_prog();
                    prog_->enqueue_after_termination(start_prog_task);
                } else {
                    start_prog_task(pm_tiny_server);
                }
            }
        }
    } else if (command == pm_tiny::control_command::version) {
        auto wf = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value<std::int32_t>(*wf, 0);
        pm_tiny::fappend_value(*wf, "success");
        pm_tiny::fappend_value(*wf, pm_tiny::pm_tiny_version);
        session->write_frame(wf);
    } else if (command == pm_tiny::control_command::log) {
        pm_tiny::program_log_request log_request;
        try {
            log_request = pm_tiny::read_program_log_request(request.payload);
        } catch (const std::exception &error) {
            auto wf = pm_tiny::make_control_response_payload(-1, error.what());
            session->write_frame(wf);
            return;
        }
        const auto &name = log_request.name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        auto wf = std::make_unique<pm_tiny::frame_t>();
        if (iter == pm_tiny_progs.end()) {
            pm_tiny::fappend_value<int>(*wf, 0x1);
            pm_tiny::fappend_value(*wf, "not found `" + name + "`");
            session->write_frame(wf);
        } else {
            auto *prog = *iter;
            bool is_alive = prog->instance.pid != -1;
            if (log_request.mode == pm_tiny::log_request_mode::live && !is_alive) {
                if (prog->restart_pending) {
                    prog->wait_for_next_log_generation(session);
                } else {
                    pm_tiny::fappend_value<int>(*wf, 0x2);
                    pm_tiny::fappend_value(*wf, "`" + name + "` is not running; use `pm log " +
                                                  name + " --history` to show the last completed generation");
                    session->write_frame(wf);
                }
            } else if (log_request.mode == pm_tiny::log_request_mode::history && is_alive) {
                pm_tiny::fappend_value<int>(*wf, 0x2);
                pm_tiny::fappend_value(*wf, "`" + name + "` is running; use `pm log " + name + "`");
                session->write_frame(wf);
            } else if (log_request.mode == pm_tiny::log_request_mode::history &&
                       (prog->instance.generation == 0 || !prog->has_last_exit ||
                        prog->last_exit_time_unix_ms <= 0)) {
                pm_tiny::fappend_value<int>(*wf, 0x2);
                pm_tiny::fappend_value(*wf, "no completed log generation available for `" + name + "`");
                session->write_frame(wf);
            } else {
                if (log_request.mode == pm_tiny::log_request_mode::live) prog->add_session(session.get());
                pm_tiny::fappend_value<int>(*wf, 0);
                pm_tiny::fappend_value(*wf, "success");
                pm_tiny::program_log_response response;
                response.mode = log_request.mode;
                response.generation = prog->instance.generation;
                response.last_pid = prog->instance.last_pid;
                response.last_exit_time_unix_ms = prog->last_exit_time_unix_ms;
                if (prog->has_last_exit) {
                    if (WIFEXITED(prog->last_wstatus)) {
                        response.exit_reason = "exited";
                        response.exit_code = WEXITSTATUS(prog->last_wstatus);
                    } else if (WIFSIGNALED(prog->last_wstatus)) {
                        response.exit_reason = "signaled";
                        response.exit_code = WTERMSIG(prog->last_wstatus);
                    } else {
                        response.exit_reason = "unknown";
                        response.exit_code = prog->last_wstatus;
                    }
                }
                pm_tiny::append_program_log_response(*wf, response);
                session->write_frame(wf);
                prog->write_cache_log_to_session(session.get());
                if (log_request.mode == pm_tiny::log_request_mode::history) {
                    auto final_frame = pm_tiny::str_to_frames(0, "");
                    session->write_stream_frame(final_frame.front(), 0, false);
                }
            }
        }

    } else if (command == pm_tiny::control_command::app_ready) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        if (iter == pm_tiny_progs.end()) {
            PM_TINY_DLOG_ERROR("not found app: `%s`", name.c_str());
            session->close();
        } else {
            auto prog = *iter;
            if (!heartbeat_peer_matches(pm_tiny_server, session, prog, "ready")) return;
            PM_TINY_DLOG_DEBUG("app `%s` ready", name.c_str());
            if (prog->state == PM_TINY_PROG_STATE_STARTING) {
                prog->state = PM_TINY_PROG_STATE_RUNING;
                prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
                proglist_t pl;
                pl.push_back(prog);
                pm_tiny_server.spawn1(pl);
            }
        }
    } else if (command == pm_tiny::control_command::app_tick) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        if (iter == pm_tiny_progs.end()) {
            PM_TINY_DLOG_ERROR("not found app: `%s`", name.c_str());
            session->close();
        } else {
            auto prog = *iter;
            if (!heartbeat_peer_matches(pm_tiny_server, session, prog, "tick")) return;
            PM_TINY_DLOG_DEBUG("recv `%s` tick", name.c_str());
            if (prog->state == PM_TINY_PROG_STATE_RUNING) {
                prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
            }
        }
    } else if (command == pm_tiny::control_command::inspect) {
        handle_cmd_inspect(pm_tiny_server, ifs, session);
    } else if (command == pm_tiny::control_command::quit) {
        auto wf = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*wf, 0);
        pm_tiny::fappend_value(*wf, "success");
        int pid = getpid();
        pm_tiny::fappend_value(*wf, pid);
        session->write_frame(wf);
        pm_tiny_server.request_quit();
    } else if (command == pm_tiny::control_command::reload) {
        if (server_exiting(pm_tiny_server, session)) {
            return;
        }
        if (pm_tiny_server.persistence_busy()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -1);
            pm_tiny::fappend_value(*wf, "persistence operation busy");
            session->write_frame(wf);
            return;
        }
        auto reload_config = pm_tiny_server.parse_cfg2();
        if (!reload_config->is_valid()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -1);
            pm_tiny::fappend_value(*wf, reload_config->error_message_.empty() ?
                                        "invalid configuration" : reload_config->error_message_);
            session->write_frame(wf);
            pm_tiny::delete_proglist(reload_config->pl_);
            reload_config->pl_.clear();
            return;
        }
        pm_tiny_server.reload_config = std::move(reload_config);
        pm_tiny_server.request_quit();
        pm_tiny_server.wait_reload_sessions.emplace_back(session);
    }
}
