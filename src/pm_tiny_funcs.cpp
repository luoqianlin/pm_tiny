//
// Created by qianlinluo@foxmail.com on 23-7-27.
//
#include "pm_tiny_funcs.h"
#include "pm_tiny_server.h"
#include "prog.h"
#include <cassert>
#include "log.h"

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

pm_tiny::frame_ptr_t make_prog_info_data(pm_tiny::proglist_t &pm_tiny_progs) {
    //n pid:name:workdir:command:restart_count:state:VmRSSkiB
    auto f = std::make_unique<pm_tiny::frame_t>();
    pm_tiny::fappend_value<int>(*f, (int) pm_tiny_progs.size());
    for (auto &prog_info: pm_tiny_progs) {
        pm_tiny::fappend_value(*f, (int) prog_info->pid);
        pm_tiny::fappend_value(*f, prog_info->name);
        pm_tiny::fappend_value(*f, prog_info->work_dir);
        std::string command = std::accumulate(prog_info->args.begin(), prog_info->args.end(),
                                              std::string(),
                                              [](const std::string &lv, const std::string &rv) {
                                                  std::string v = lv + " ";
                                                  v += rv;
                                                  return v;
                                              });
        mgr::utils::trim(command);
        pm_tiny::fappend_value(*f, command);
        pm_tiny::fappend_value(*f, prog_info->dead_count);
        pm_tiny::fappend_value(*f, prog_info->state);
        long long VmRSSkiB = 0;
        if (prog_info->pid > 0) {
            VmRSSkiB = pm_tiny::get_vm_rss_kib(prog_info->pid);
        }
        pm_tiny::fappend_value(*f, VmRSSkiB);
        int daemon = prog_info->daemon ? 1 : 0;
        pm_tiny::fappend_value(*f, daemon);
        auto &depends_on = prog_info->depends_on;
        pm_tiny::fappend_value(*f, static_cast<int>(depends_on.size()));
        for (auto &depd: depends_on) {
            pm_tiny::fappend_value(*f, depd);
        }
    }
    return f;
}

std::unique_ptr<pm_tiny::frame_t> handle_cmd_start(pm_tiny::pm_tiny_server_t &pm_tiny_server,
                                                   pm_tiny::iframe_stream &ifs,
                                                   std::shared_ptr<pm_tiny::session_t> &session) {
    using pm_tiny::proglist_t;
    using pm_tiny::prog_ptr_t;
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    //name:cwd:command
    std::string name;
    std::string cwd;
    std::string command;
    int local_resolved;
    int env_num;
    int kill_timeout;
    std::string run_as;
    int depends_size = 0;
    std::vector<std::string> depends_on;
    int start_timeout;
    pm_tiny::failure_action_underlying_t failure_action_underly;
    int daemon;
    int heartbeat_timeout;
    int show_log = 0;
    ifs >> name >> cwd >> command >> local_resolved >> env_num;
    std::vector<std::string> envs;
    envs.resize(env_num);
    for (int k = 0; k < env_num; k++) {
        ifs >> envs[k];
    }
    ifs >> kill_timeout;
    ifs >> run_as;
    ifs >> show_log;
    ifs >> depends_size;
    depends_on.resize(depends_size);
    for (int i = 0; i < depends_size; i++) {
        ifs >> depends_on[i];
    }
    ifs >> start_timeout;
    ifs >> failure_action_underly;
    ifs >> daemon >> heartbeat_timeout;
//   std::cout << "name:`" + name << "` cwd:`" << cwd << "` command:`" << command
//   << "` local_resolved:" << local_resolved << std::endl;
//    PM_TINY_LOG_D("run_as:%s",run_as.c_str());
    auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                             [&name](const prog_ptr_t &prog) {
                                 return prog->name == name;
                             });
    auto wf = std::make_unique<pm_tiny::frame_t>();
    auto prog = pm_tiny_server.create_prog(name, cwd, command, envs, kill_timeout, run_as);
    if (prog) {
        prog->depends_on = depends_on;
        prog->start_timeout = start_timeout;
        prog->failure_action = static_cast<pm_tiny::failure_action_t>(failure_action_underly);
        prog->daemon = (daemon != 0);
        prog->heartbeat_timeout = heartbeat_timeout;
    }
    if (iter == pm_tiny_progs.end()) {
        if (!prog) {
            pm_tiny::fappend_value<int>(*wf, -0x3);
            pm_tiny::fappend_value(*wf, "create `" + name + "` fail");
        } else {
            auto is_valid = pm_tiny_server.is_prog_depends_valid(prog.get());
            if (!is_valid) {
                pm_tiny::fappend_value<int>(*wf, -0x3);
                pm_tiny::fappend_value(*wf, "create `" + name + "` fail,depends_on invalid.");
            } else {
                int ret = pm_tiny_server.start_and_add_prog(prog.get());
                if (ret == -1) {
                    std::string errmsg(strerror(errno));
                    pm_tiny::fappend_value<int>(*wf, -1);
                    pm_tiny::fappend_value(*wf, errmsg);
                } else {
                    if (show_log) {
                        auto prog_pointer = prog.get();
                        prog_bind_session(session, prog_pointer, wf);
                    } else {
                        pm_tiny::fappend_value<int>(*wf, 0);
                        pm_tiny::fappend_value(*wf, "success");
                    }
                    (void *) prog.release();
                }
            }
        }
    } else {
        prog_ptr_t _p = *iter;
        if (_p->pid == -1) {
//            auto prog = pm_tiny_server.create_prog(name, cwd, command,
//                                                   envs, kill_timeout, run_as);
            if (local_resolved && prog
                && (!_p->is_cfg_equal(prog.get()))) {
                pm_tiny::fappend_value<int>(*wf, -4);
                pm_tiny::fappend_value(*wf, "The cwd or command or environ or depends_on has changed,"
                                            " please run the delete operation first");
            } else {
                _p->envs = envs;
                int rc = pm_tiny_server.start_prog(_p);
                if (rc == -1) {
                    std::string errmsg(strerror(errno));
                    pm_tiny::fappend_value<int>(*wf, -1);
                    pm_tiny::fappend_value(*wf, errmsg);
                } else {
                    if (show_log) {
                        prog_bind_session(session, _p, wf);
                    } else {
                        pm_tiny::fappend_value<int>(*wf, 0);
                        pm_tiny::fappend_value(*wf, "success");
                    }
                }
            }
        } else {
            pm_tiny::fappend_value<int>(*wf, -2);
            pm_tiny::fappend_value(*wf, "`" + name + "` already running");
        }
    }
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
        pm_tiny::fappend_value(*f, prog_info->name);
        pm_tiny::fappend_value(*f, prog_info->work_dir);
        std::string command = std::accumulate(prog_info->args.begin(), prog_info->args.end(),
                                              std::string(),
                                              [](const std::string &lv, const std::string &rv) {
                                                  std::string v = lv + " ";
                                                  v += rv;
                                                  return v;
                                              });
        mgr::utils::trim(command);
        pm_tiny::fappend_value(*f, command);
        int daemon = prog_info->daemon ? 1 : 0;
        pm_tiny::fappend_value(*f, daemon);
        auto &depends_on = prog_info->depends_on;
        pm_tiny::fappend_value(*f, static_cast<int>(depends_on.size()));
        for (auto &depd: depends_on) {
            pm_tiny::fappend_value(*f, depd);
        }
        pm_tiny::fappend_value(*f, prog_info->start_timeout);
        pm_tiny::fappend_value(*f, static_cast<std::underlying_type_t<
                pm_tiny::failure_action_t> >(prog_info->failure_action));
        pm_tiny::fappend_value(*f, prog_info->heartbeat_timeout);
        pm_tiny::fappend_value(*f, prog_info->kill_timeout_sec);
        pm_tiny::fappend_value(*f, prog_info->run_as);
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
#if PM_TINY_PTY_ENABLE
    prog->add_session(session.get());
    pm_tiny::fappend_value<int>(*wf, 1);
    pm_tiny::fappend_value(*wf, "success");
#else
    pm_tiny::fappend_value<int>(*wf, -2);
    pm_tiny::fappend_value(*wf, "This version does not support log display");
#endif
}

void handle_frame(pm_tiny_server_t &pm_tiny_server, pm_tiny::frame_ptr_t &rf,
                  pm_tiny::session_ptr_t &session) {
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    pm_tiny::iframe_stream ifs(*rf);
    uint8_t f_type;
    ifs >> f_type;
    if (f_type == 0x23) {//ls
        pm_tiny::frame_ptr_t f = make_prog_info_data(pm_tiny_progs);
        session->write_frame(f);
    } else if (f_type == 0x24) {//stop
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
                if (prog_->pid != -1) {
                    if (server_exiting(pm_tiny_server, session)) {
                        return;
                    }
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
                    pm_tiny_server.async_kill_prog(prog_);
                    prog_->kill_pendingtasks.emplace_back(stop_proc_task);
                } else {
                    auto wf = std::make_unique<pm_tiny::frame_t>();
                    pm_tiny::fappend_value<int>(*wf, -2);
                    pm_tiny::fappend_value(*wf, "`" + name + "` not running");
                    session->write_frame(wf);
                }
            }
        }

    } else if (f_type == 0x25) {//start
        if (server_exiting(pm_tiny_server, session)) {
            return;
        }
        auto wf = handle_cmd_start(pm_tiny_server, ifs, session);
        session->write_frame(wf);
    } else if (f_type == 0x26) {//save
        auto wf = std::make_unique<pm_tiny::frame_t>();
        int ret = pm_tiny_server.save_proc_to_cfg();
        if (ret == 0) {
            pm_tiny::fappend_value<int>(*wf, 0);
            pm_tiny::fappend_value(*wf, "save success");
        } else {
            pm_tiny::fappend_value<int>(*wf, 1);
            pm_tiny::fappend_value(*wf, "save fail");
        }
        session->write_frame(wf);
    } else if (f_type == 0x27) {//delete
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

                if (prog_->pid != -1) {
                    pm_tiny_server.async_kill_prog(prog_);
                    prog_->state = PM_TINY_PROG_STATE_REQUEST_DELETE;
                    prog_->kill_pendingtasks.emplace_back(delete_prog_task);
                } else {
                    pm_tiny_server.trigger_DAG_traversal_next_node(prog_);
                    delete_prog_task(pm_tiny_server);
                    pm_tiny_server.remove_prog(prog_);
                }
            }
        }

    } else if (f_type == 0x28) {//restart
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
            if (!prog_->kill_pendingtasks.empty()) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                pm_tiny::fappend_value<int>(*wf, -0x3);
                std::string msg = msg_cmd_not_completed(name);
                pm_tiny::fappend_value(*wf, msg);
                session->write_frame(wf);
            } else {
                bool is_alive = prog_->pid != -1;
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
                    pm_tiny_server.async_kill_prog(prog_);
                    prog_->kill_pendingtasks.emplace_back(start_prog_task);
                } else {
                    start_prog_task(pm_tiny_server);
                }
            }
        }
    } else if (f_type == 0x29) {//version
        auto wf = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value(*wf, pm_tiny::pm_tiny_version);
        session->write_frame(wf);
    } else if (f_type == PM_TINY_FRAME_TYPE_SHOW_LOG) {
        std::string name;
        ifs >> name;
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
            bool is_alive = (*iter)->pid != -1;
            if (!is_alive) {
                pm_tiny::fappend_value<int>(*wf, 0x2);
                pm_tiny::fappend_value(*wf, "`" + name + "` not running");
                session->write_frame(wf);
            } else {
#if PM_TINY_PTY_ENABLE
                (*iter)->add_session(session.get());
                pm_tiny::fappend_value<int>(*wf, 0);
                pm_tiny::fappend_value(*wf, "success");
                session->write_frame(wf);
                (*iter)->write_cache_log_to_session(session.get());
#else
                pm_tiny::fappend_value<int>(*wf, 0x4);
                            pm_tiny::fappend_value(*wf, "This version does not support this function");
                            session->write_frame(wf);
#endif
            }
        }

    } else if (f_type == PM_TINY_FRAME_TYPE_APP_READY) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        if (iter == pm_tiny_progs.end()) {
            PM_TINY_LOG_E("not found app: `%s`", name.c_str());
            session->close();
        } else {
            PM_TINY_LOG_D("app `%s` ready", name.c_str());
            auto prog = *iter;
            if (prog->pid != -1 && prog->state == PM_TINY_PROG_STATE_STARTING) {
                prog->state = PM_TINY_PROG_STATE_RUNING;
                prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
                proglist_t pl;
                pl.push_back(prog);
                pm_tiny_server.spawn1(pl);
            }
        }
    } else if (f_type == PM_TINY_FRAME_TYPE_APP_TICK) {
        std::string name;
        ifs >> name;
        auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                 [&name](const prog_ptr_t &prog) {
                                     return prog->name == name;
                                 });
        if (iter == pm_tiny_progs.end()) {
            PM_TINY_LOG_E("not found app: `%s`", name.c_str());
            session->close();
        } else {
//                        PM_TINY_LOG_D("recv `%s` tick", name.c_str());
            auto prog = *iter;
            if (prog->pid != -1 && prog->state == PM_TINY_PROG_STATE_RUNING) {
                prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
            }
        }
    } else if (f_type == PM_TINY_FRAME_TYPE_APP_INSPECT) {//inspect
        handle_cmd_inspect(pm_tiny_server, ifs, session);
    } else if (f_type == PM_TINY_FRAME_TYPE_QUIT) {
        auto wf = std::make_unique<pm_tiny::frame_t>();
        pm_tiny::fappend_value<int>(*wf, 0);
        pm_tiny::fappend_value(*wf, "success");
        int pid = getpid();
        pm_tiny::fappend_value(*wf, pid);
        session->write_frame(wf);
        pm_tiny_server.request_quit();
    } else if (f_type == PM_TINY_FRAME_TYPE_RELOAD) {
        if (server_exiting(pm_tiny_server, session)) {
            return;
        }
        auto reload_config = pm_tiny_server.parse_cfg2();
        if (!reload_config->is_valid()) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, -1);
            pm_tiny::fappend_value(*wf, "invalid configuration");
            session->write_frame(wf);
            pm_tiny::delete_proglist(reload_config->pl_);
            reload_config->pl_.clear();
            return;
        }
        pm_tiny_server.reload_config = std::move(reload_config);
        pm_tiny_server.request_quit();
        pm_tiny_server.wait_reload_sessions.emplace_back(session);
    } else {
        PM_TINY_LOG_I("unkown framae type:%#02X", f_type);
    }
}