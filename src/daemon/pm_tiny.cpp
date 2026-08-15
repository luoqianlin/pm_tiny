#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include "pm_tiny_server.h"
#include "asio_daemon_loop.h"
#include "daemon_log.h"
#include "time_util.h"
#include "prog.h"
#include "assert.h"
#include "ScopeGuard.h"
#include "pm_tiny_funcs.h"
#include "pm_sys.h"
#include "android_lmkd.h"
#include "process_reaper.h"
#include "daemon_config.h"

using prog_ptr_t = pm_tiny::prog_ptr_t;
using proglist_t = pm_tiny::proglist_t;
using pm_tiny_server_t = pm_tiny::pm_tiny_server_t;

size_t get_living_processes_count(proglist_t &pm_tiny_progs);

static volatile sig_atomic_t exit_signal = 0;
static volatile sig_atomic_t alarm_signal = 0;
static volatile sig_atomic_t hup_signal = 0;
static volatile sig_atomic_t exit_chld_signal = 0;
static volatile sig_atomic_t signal_wakeup_fd = -1;

static void wake_event_loop() {
    if (signal_wakeup_fd < 0) return;
    const char byte = 1;
    const int saved_errno = errno;
    (void)::write(static_cast<int>(signal_wakeup_fd), &byte, 1);
    errno = saved_errno;
}

void sig_exit_handler(int sig, siginfo_t *, void *) {
    exit_signal = sig;
    wake_event_loop();
}

void sig_chld_handler(int sig, siginfo_t *, void *) {
    exit_chld_signal = sig;
    wake_event_loop();
}

void sig_alarm_handler(int sig, siginfo_t *, void *) {
    alarm_signal = sig;
    wake_event_loop();
}

void sig_hup_handler(int sig, siginfo_t *, void *) {
    hup_signal = sig;
    wake_event_loop();
}

static sig_atomic_t take_pending_signal(volatile sig_atomic_t &pending, int signo) {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, signo);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask block signal %d", signo);
        return 0;
    }
    sig_atomic_t saved = pending;
    pending = 0;
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask restore signal %d", signo);
        std::exit(EXIT_FAILURE);
    }
    return saved;
}

static sig_atomic_t take_pending_exit_signal() {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask block exit signals");
        return 0;
    }
    sig_atomic_t saved = exit_signal;
    exit_signal = 0;
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask restore exit signals");
        std::exit(EXIT_FAILURE);
    }
    return saved;
}

static void check_delayed_exit_sig(pm_tiny_server_t &tiny_server) {
//    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_exit_signal = take_pending_exit_signal();
    if (save_exit_signal) {
        pm_tiny::daemon_log_signal(save_exit_signal);
        bool terminate = save_exit_signal == SIGTERM
                         || save_exit_signal == SIGINT;
        if (terminate) {
//            kill_prog(pm_tiny_progs);
            tiny_server.request_quit();
        }
    }
}

/*void kill_prog(prog_ptr_t prog) {
    bool find = xx_kill_1(prog, SIGTERM);
    if (find) {
        pm_tiny::sleep_waitfor(prog->kill_timeout_sec, [&find, &prog]() {
            find = xx_wait_1(prog, WNOHANG);
            return !find;
        });
        if (find) {
            PM_TINY_DLOG_INFO("force kill %s(pid:%d)", prog->name.c_str(), prog->instance.pid);
            xx_kill_1(prog, SIGKILL);
            pm_tiny::sleep_waitfor(1, [&find, &prog]() {
                find = xx_wait_1(prog, 0);
                return !find;
            });
        }
    }
    prog->close_fds();
}*/



/*
 * 检查是否有子进程退出，如果有则回收子进程空间(waitpid)
 * */
void check_delayed_chld_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    int wstatus, rc;
    sig_atomic_t save_exit_chld_signal = take_pending_signal(exit_chld_signal, SIGCHLD);
    if (save_exit_chld_signal) {
        pm_tiny::daemon_log_signal(save_exit_chld_signal);
        proglist_t starting_prog;
        proglist_t penddingtask_progs;
        proglist_t deleting_progs;
        pm_tiny::process_reaper reaper;
        for (const auto &child : reaper.reap_all()) {
            rc = child.pid;
            wstatus = child.status;
            auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                     [rc](const prog_ptr_t &v) { return v->instance.pid == rc; });

            if (iter != pm_tiny_progs.end()) {
                auto p = *iter;
                    tiny_server.mark_dependency_stopped(p);
                    std::string exit_info = pm_tiny::prog_info_t::log_proc_exit_status(&(*p), rc, wstatus);
                    PM_TINY_DLOG_INFO("%s", exit_info.c_str());
                    auto now_ms = p->update_count_timer();
                    auto life_time = now_ms - p->last_startup_ms;
                    p->last_wstatus = wstatus;
                    p->has_last_exit = true;
                    p->close_fds(tiny_server.lmkdFd);
                    const bool tree_empty = p->is_tree_empty();
                    if (!tree_empty) {
                        PM_TINY_DLOG_ERROR("`%s` root exited while descendants remain; terminating process tree",
                                      p->name.c_str());
                        const bool should_restart = p->daemon &&
                            p->state != PM_TINY_PROG_STATE_REQUEST_STOP &&
                            p->state != PM_TINY_PROG_STATE_REQUEST_DELETE;
                        if (should_restart) {
                            p->enqueue_after_termination([p, now_ms, life_time](pm_tiny_server_t &) {
                                const auto decision = p->plan_automatic_restart(now_ms, life_time);
                                if (decision.restart) {
                                    p->state = PM_TINY_PROG_STATE_WAITING_START;
                                    PM_TINY_DLOG_INFO("`%s` restart scheduled in %dms (attempt %d)",
                                                  p->name.c_str(), decision.delay_ms,
                                                  decision.attempts_in_window);
                                } else {
                                    p->state = PM_TINY_PROG_STATE_STOPED;
                                    PM_TINY_DLOG_ERROR("`%s` automatic restart suppressed after %d attempts",
                                                  p->name.c_str(), decision.attempts_in_window);
                                }
                            });
                        }
                        if (p->mark_tree_draining() == pm_tiny::termination_action::send_term) {
                            p->signal_tree(SIGTERM);
                        }
                        p->state = PM_TINY_PROG_STATE_REQUEST_STOP;
                        p->instance.termination = pm_tiny::termination_phase::tree_draining;
                        continue;
                    }
                    p->tree_controller->cleanup(p->instance.tree);
                    if (p->state == PM_TINY_PROG_STATE_REQUEST_STOP ||
                        p->state == PM_TINY_PROG_STATE_REQUEST_DELETE) {
                        penddingtask_progs.push_back(p);
                    }
                    if (p->state == PM_TINY_PROG_STATE_REQUEST_DELETE) deleting_progs.push_back(p);
                    bool normal_exit = p->state == PM_TINY_PROG_STATE_REQUEST_STOP
                                       || p->state == PM_TINY_PROG_STATE_REQUEST_DELETE
                                       || !p->daemon;
                    if (!normal_exit) {
                        const auto decision = p->plan_automatic_restart(now_ms, life_time);
                        if (decision.restart) {
                            p->dead_count++;
                            p->state = PM_TINY_PROG_STATE_WAITING_START;
                            PM_TINY_DLOG_INFO("`%s` restart scheduled in %dms (attempt %d)",
                                          p->name.c_str(), decision.delay_ms,
                                          decision.attempts_in_window);
                        } else {
                            if (p->state == PM_TINY_PROG_STATE_STARTING) {
                                starting_prog.push_back(p);
                            }
                            PM_TINY_DLOG_ERROR("`%s` automatic restart suppressed after %d attempts",
                                          p->name.c_str(), decision.attempts_in_window);
                            p->set_state(PM_TINY_PROG_STATE_STOPED);
                        }
                    } else {
                        if (p->state == PM_TINY_PROG_STATE_STARTING) {
                            starting_prog.push_back(p);
                        }
                        if (p->state == PM_TINY_PROG_STATE_REQUEST_STOP) {
                            p->set_state(PM_TINY_PROG_STATE_STOPED);
                        } else {
                            p->set_state(PM_TINY_PROG_STATE_EXIT);
                        }
                    }
            } else {
                PM_TINY_DLOG_INFO("%s", pm_tiny::describe_reaped_descendant(child).c_str());
            }

        }
        if (!starting_prog.empty()) {
            tiny_server.spawn1(starting_prog);
        }
        for (auto &p: penddingtask_progs) {
            p->execute_penddingtasks(tiny_server);
        }
        for (auto &p: deleting_progs) {
            tiny_server.remove_prog(p);
        }
    }
    proglist_t starting_prog;
    bool reboot = false;
    for (auto &prog: pm_tiny_progs) {
        if (prog->restart_pending && prog->instance.pid == -1 &&
            prog->is_tree_empty() && !tiny_server.is_exiting() &&
            pm_tiny::time::gettime_monotonic_ms() >= prog->restart_due_ms) {
            int ret = tiny_server.start_prog(prog);
            prog->restart_pending = false;
            prog->restart_due_ms = 0;
            if (ret == -1) {
                PM_TINY_DLOG_ERROR_ERRNO("scheduled restart `%s` failed", prog->name.c_str());
                tiny_server.flag_startup_fail(prog);
            }
            continue;
        }
        if (prog->state == PM_TINY_PROG_STATE_REQUEST_STOP
            || prog->state == PM_TINY_PROG_STATE_REQUEST_DELETE) {
            auto termination_action = prog->poll_termination();
            if (prog->instance.pid == -1 && prog->is_tree_empty()) {
                prog->tree_controller->cleanup(prog->instance.tree);
                prog->instance.termination = pm_tiny::termination_phase::completed;
                prog->execute_penddingtasks(tiny_server);
            } else if (termination_action == pm_tiny::termination_action::send_kill) {
                prog->async_force_kill();
            }
        } else {
            if (tiny_server.is_exiting()) {
                continue;
            }
            if (prog->state == PM_TINY_PROG_STATE_STARTING) {
                if (prog->instance.pid != -1 && prog->is_start_timeout()) {
                    if (prog->failure_action == pm_tiny::failure_action_t::SKIP
                        || prog->start_timeout == 0) {
                        starting_prog.push_back(prog);
                        prog->state = PM_TINY_PROG_STATE_RUNING;
                        prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
                        PM_TINY_DLOG_DEBUG("start timeout:%s", prog->name.c_str());
                    } else if (prog->failure_action == pm_tiny::failure_action_t::RESTART) {
                        prog->async_kill_prog();
                        auto start_prog_task =
                                [&prog](pm_tiny_server_t &) {
                                    const auto now_ms = pm_tiny::time::gettime_monotonic_ms();
                                    const auto runtime_ms = now_ms - prog->last_startup_ms;
                                    const auto decision = prog->plan_automatic_restart(now_ms, runtime_ms);
                                    if (decision.restart) {
                                        ++prog->dead_count;
                                        prog->state = PM_TINY_PROG_STATE_WAITING_START;
                                        PM_TINY_DLOG_INFO("`%s` timeout restart scheduled in %dms (attempt %d)",
                                                      prog->name.c_str(), decision.delay_ms,
                                                      decision.attempts_in_window);
                                    } else {
                                        prog->state = PM_TINY_PROG_STATE_STOPED;
                                        PM_TINY_DLOG_ERROR("`%s` timeout restart suppressed after %d attempts",
                                                      prog->name.c_str(), decision.attempts_in_window);
                                    }
                                };
                        prog->enqueue_after_termination(start_prog_task);
                    } else {
                        PM_TINY_DLOG_INFO("`%s` start timeout reboot now.", prog->name.c_str());
                        pm_tiny::process_reboot();
                        reboot = true;
                        break;
                    }
                }
            } else if (prog->state == PM_TINY_PROG_STATE_RUNING) {
                if (prog->instance.pid != -1 && prog->is_tick_timeout()) {
                    if (prog->failure_action == pm_tiny::failure_action_t::RESTART) {
                        PM_TINY_DLOG_INFO("`%s` tick timeout restart now.", prog->name.c_str());
                        prog->async_kill_prog();
                        auto start_prog_task =
                                [&prog](pm_tiny_server_t &) {
                                    const auto now_ms = pm_tiny::time::gettime_monotonic_ms();
                                    const auto runtime_ms = now_ms - prog->last_startup_ms;
                                    const auto decision = prog->plan_automatic_restart(now_ms, runtime_ms);
                                    if (decision.restart) {
                                        ++prog->dead_count;
                                        prog->state = PM_TINY_PROG_STATE_WAITING_START;
                                        PM_TINY_DLOG_INFO("`%s` timeout restart scheduled in %dms (attempt %d)",
                                                      prog->name.c_str(), decision.delay_ms,
                                                      decision.attempts_in_window);
                                    } else {
                                        prog->state = PM_TINY_PROG_STATE_STOPED;
                                        PM_TINY_DLOG_ERROR("`%s` timeout restart suppressed after %d attempts",
                                                      prog->name.c_str(), decision.attempts_in_window);
                                    }
                                };
                        prog->enqueue_after_termination(start_prog_task);
                    } else if (prog->failure_action == pm_tiny::failure_action_t::REBOOT) {
                        PM_TINY_DLOG_INFO("`%s` tick timeout reboot now.", prog->name.c_str());
                        pm_tiny::process_reboot();
                        reboot = true;
                        break;
                    } else if (prog->failure_action == pm_tiny::failure_action_t::SKIP) {
//                    PM_TINY_DLOG_INFO("`%s` tick timeout skip.", prog->name.c_str());
                    }
                }
            }
        }
    }

    if (!reboot && !starting_prog.empty()) {
        tiny_server.spawn1(starting_prog);
    }
}

size_t get_living_processes_count(proglist_t &pm_tiny_progs) {
    size_t wait_proc_num = std::count_if(pm_tiny_progs.cbegin(), pm_tiny_progs.cend(),
                                         [](const prog_ptr_t &v) {
                                             return v->instance.pid != -1 || !v->is_tree_empty();
                                         });
    return wait_proc_num;
}

void check_delayed_sigs(pm_tiny_server_t &tiny_server) {
    check_delayed_exit_sig(tiny_server);
    check_delayed_chld_sig(tiny_server);
    sig_atomic_t saved_hup = take_pending_signal(hup_signal, SIGHUP);
    if (saved_hup) pm_tiny::daemon_log_signal(saved_hup);
    sig_atomic_t saved_alarm = take_pending_signal(alarm_signal, SIGALRM);
    if (saved_alarm) pm_tiny::daemon_log_signal(saved_alarm);
}

void install_signal_handler(int signo, void (*handler)(int, siginfo_t *, void *)) {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = handler;
    if (sigaction(signo, &act, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigaction %d", signo);
        std::exit(EXIT_FAILURE);
    }
}

void install_signal_handlers() {
    install_signal_handler(SIGTERM, sig_exit_handler);
    install_signal_handler(SIGINT, sig_exit_handler);
    install_signal_handler(SIGCHLD, sig_chld_handler);
    install_signal_handler(SIGHUP, sig_hup_handler);
    install_signal_handler(SIGALRM, sig_alarm_handler);
}

int next_maintenance_delay_ms(const pm_tiny_server_t &server) {
    if (exit_signal || alarm_signal || hup_signal || exit_chld_signal) return 0;
    if (server.is_exiting()) return 25;
    if (server.persistence_busy()) return 10;
    const int64_t now = pm_tiny::time::gettime_monotonic_ms();
    int64_t next_due = now + 1000;
    bool tree_draining = false;
    for (const auto &prog : server.pm_tiny_progs) {
        if (prog->restart_pending) next_due = std::min(next_due, prog->restart_due_ms);
        if (prog->state == PM_TINY_PROG_STATE_STARTING && prog->instance.pid != -1 &&
            prog->start_timeout >= 0) {
            next_due = std::min(next_due, prog->last_startup_ms +
                static_cast<int64_t>(prog->start_timeout) * 1000);
        }
        if (prog->state == PM_TINY_PROG_STATE_RUNING && prog->heartbeat_timeout > 0) {
            next_due = std::min(next_due, prog->last_tick_timepoint +
                static_cast<int64_t>(prog->heartbeat_timeout) * 1000);
        }
        const auto phase = prog->instance.job.phase();
        if (phase == pm_tiny::termination_phase::term_requested) {
            next_due = std::min(next_due, prog->instance.job.deadline_ms());
        } else if (phase == pm_tiny::termination_phase::tree_draining ||
                   phase == pm_tiny::termination_phase::force_kill_requested) {
            tree_draining = true;
        }
    }
    if (tree_draining) return 25;
    if (next_due <= now) return 1;
    return static_cast<int>(std::min<int64_t>(1000, next_due - now));
}

int open_uds_listen_fd(const std::string &sock_path
                       ,bool enable_abstract_namespace) {
    const std::size_t max_path = sizeof(((struct sockaddr_un *)nullptr)->sun_path);
    if ((enable_abstract_namespace && sock_path.size() > max_path - 2) ||
        (!enable_abstract_namespace && sock_path.size() >= max_path)) {
        PM_TINY_DLOG_ERROR("Unix socket address is too long (%zu bytes)", sock_path.size());
        errno = ENAMETOOLONG;
        return -1;
    }
    int sfd;
    struct sockaddr_un my_addr{};
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("socket");
        std::exit(EXIT_FAILURE);
    };

    memset(&my_addr, 0, sizeof(struct sockaddr_un));
    my_addr.sun_family = AF_UNIX;
    socklen_t addr_length;
    if (enable_abstract_namespace) {
        my_addr.sun_path[0] = '\0';
        strncpy(my_addr.sun_path + 1, sock_path.c_str(), sizeof(my_addr.sun_path) - 2);
        addr_length = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + sock_path.length() + 1);
    } else {
        strncpy(my_addr.sun_path, sock_path.c_str(),
                sizeof(my_addr.sun_path) - 1);
        addr_length = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + sock_path.length() + 1);
    }
    int rc = pm_tiny::set_nonblock(sfd);
    if (rc < 0) {
        PM_TINY_DLOG_ERROR_ERRNO("fcntl");
        std::exit(EXIT_FAILURE);
    }
    rc = pm_tiny::set_cloexec(sfd);
    if (rc < 0) {
        PM_TINY_DLOG_ERROR_ERRNO("set_cloexec");
        std::exit(EXIT_FAILURE);
    }
    if (bind(sfd, (struct sockaddr *) &my_addr,addr_length) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("bind");
        std::exit(EXIT_FAILURE);
    }

    if (listen(sfd, 5) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("listen");
        std::exit(EXIT_FAILURE);
    }
    return sfd;
}

int check_quit_or_reload(pm_tiny_server_t &pm_tiny_server) {
    int save_result = -1;
    if (pm_tiny_server.poll_save_proc_to_cfg(save_result)) {
        for (auto &weak_session : pm_tiny_server.wait_save_sessions) {
            auto session = weak_session.lock();
            if (!session || session->is_close()) continue;
            auto response = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*response, save_result == 0 ? 0 : 1);
            pm_tiny::fappend_value(*response, save_result == 0 ? "save success" : "save fail");
            session->write_frame(response);
        }
        pm_tiny_server.wait_save_sessions.clear();
    }
    auto pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    auto living_processes_count = get_living_processes_count(pm_tiny_progs);
    if (pm_tiny_server.is_exiting()) {
//        PM_TINY_DLOG_DEBUG("==>prog_num:%d", living_processes_count);
        if (living_processes_count == 0 && !pm_tiny_server.persistence_busy()) {
            if (!pm_tiny_server.reload_config) {
                return 1;
            }
            pm_tiny_server.server_exit = 0;
            int code = 0;
            std::string msg = "success";
            if (!pm_tiny_server.reload_config->is_valid()) {
                code = -1;
                msg = "invalid configuration";
            }
            for (auto &wk: pm_tiny_server.wait_reload_sessions) {
                auto session = wk.lock();
                if (session && !session->is_close()) {
                    auto wf = std::make_unique<pm_tiny::frame_t>();
                    pm_tiny::fappend_value<int>(*wf, code);
                    pm_tiny::fappend_value(*wf, msg);
                    session->write_frame(wf);
                }
            }
            pm_tiny_server.wait_reload_sessions.clear();
            if (code == -1) {
                pm_tiny::delete_proglist(pm_tiny_server.reload_config->pl_);
                pm_tiny_server.reload_config->pl_.clear();
                return 0;
            }
            pm_tiny_server.swap_reload_config();
            delete_proglist(pm_tiny_progs);
            pm_tiny_server.show_prog_depends_info();
            pm_tiny_server.spawn();
        }
    }
    return 0;
}

void start(pm_tiny_server_t &pm_tiny_server) {
    PM_TINY_DLOG_DEBUG("pm_tiny pid:%d\n", getpid());
    int rc = 0;
    auto sock_path = pm_tiny_server.pm_tiny_sock_file;
    if (!pm_tiny_server.uds_abstract_namespace) {
        unlink(sock_path.c_str());
    }
    rc = pm_tiny::set_sigaction(SIGPIPE, SIG_IGN);
    if (rc == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigaction SIGPIPE");
        std::exit(EXIT_FAILURE);
    }
    int sock_fd = open_uds_listen_fd(sock_path,
                                     pm_tiny_server.uds_abstract_namespace);
    rc = pm_tiny_server.parse_cfg();
    if (rc != 0) {
        exit(EXIT_FAILURE);
    }

    pm_tiny_server.show_prog_depends_info();

    int signal_pipe[2]{-1, -1};
    if (::pipe(signal_pipe) != 0 || pm_tiny::set_nonblock(signal_pipe[0]) != 0 ||
        pm_tiny::set_nonblock(signal_pipe[1]) != 0 || pm_tiny::set_cloexec(signal_pipe[0]) != 0 ||
        pm_tiny::set_cloexec(signal_pipe[1]) != 0) {
        PM_TINY_DLOG_ERROR_ERRNO("create signal wakeup pipe");
        std::exit(EXIT_FAILURE);
    }
    signal_wakeup_fd = signal_pipe[1];
    install_signal_handlers();
    pm_tiny_server.spawn();
    std::vector<pm_tiny::session_ptr_t> &sessions = pm_tiny_server.sessions;
    sigset_t osigmask;
    rc = sigprocmask(SIG_SETMASK, nullptr, &osigmask);
    if (rc == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask");
        std::exit(EXIT_FAILURE);
    }
    if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask");
        std::exit(EXIT_FAILURE);
    }
    auto maintenance = [&pm_tiny_server]() -> int {
        check_delayed_sigs(pm_tiny_server);
        if (check_quit_or_reload(pm_tiny_server) != 0) return -1;
        return next_maintenance_delay_ms(pm_tiny_server);
    };
    pm_tiny::asio_daemon_loop event_loop(pm_tiny_server, sock_fd, signal_pipe[0], maintenance);
    event_loop.run();
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGTERM);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGCHLD);
    sigaddset(&shutdown_signals, SIGHUP);
    sigaddset(&shutdown_signals, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &shutdown_signals, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("block signals before wakeup pipe shutdown");
        std::exit(EXIT_FAILURE);
    }
    signal_wakeup_fd = -1;
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    pm_tiny_server.wait_for_persistence();
    if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
        PM_TINY_DLOG_ERROR_ERRNO("sigprocmask");
        std::exit(EXIT_FAILURE);
    }
    auto &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    pm_tiny_server.kill_all_prog();
    delete_proglist(pm_tiny_progs);
    pm_tiny_progs.clear();
    close(sock_fd);
    if (!sessions.empty()) {
        std::for_each(sessions.begin(), sessions.end(),
                      [](const pm_tiny::session_ptr_t &session) {
                          session->close();
                      });
        sessions.clear();
    }
    if (!pm_tiny_server.uds_abstract_namespace) {
        unlink(sock_path.c_str());
    }
    PM_TINY_DLOG_INFO("pm_tiny exit");
}

int create_lock_pid_file(const char *filepath) {
    char str[20];
    int lfp = open(filepath,
                   O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (lfp < 0) {
        perror("open");
        return -1; /* can not open */
    }
    if (lockf(lfp, F_TLOCK, 0) < 0) {
        perror("lockf");
        return -1; /* can not lock */
    }
    /* first instance continues */
    sprintf(str, "%d", getpid());
    auto rc = write(lfp, str, strlen(str)); /* record pid to lockfile */
    if (rc == -1) {
        perror("write pid fail");
        return -1;
    }
    return lfp;
}

void delete_lock_pid_file(int lock_fp, const char *filepath) {
    if (lock_fp != -1) {
        if (lockf(lock_fp, F_ULOCK, 0) < 0) {
            perror("ulockf");
        }
        close(lock_fp);
        unlink(filepath);
    }
}


static void daemonize() {
    int i;
    if (getppid() == 1) {
        return; /* already a daemon */
    }
    i = fork();
    if (i < 0) exit(1); /* fork error */
    if (i > 0) exit(0); /* parent exits */
    /* child (daemon) continues */
    setsid(); /* obtain a new process group */
    pm_tiny::close_all_fds();/* close all descriptors */
    i = open("/dev/null", O_RDWR);
    auto rc = dup(i);
    if (rc == -1) {
        perror("dup fail");
    }
    rc = dup(i); /* handle standart I/O */
    if (rc == -1) {
        perror("dup fail");
    }
    umask(027); /* set newly created file permissions */
    rc = chdir("/");
    if (rc == -1) {
        perror("chdir");
    }
}

static bool create_directory_tree(const std::string &path) {
    if (path.empty()) return false;
    std::string current;
    std::size_t offset = 0;
    if (path.front() == '/') {
        current = "/";
        offset = 1;
    }
    while (offset <= path.size()) {
        const auto slash = path.find('/', offset);
        const std::string part = path.substr(offset, slash == std::string::npos ?
                                                     std::string::npos : slash - offset);
        if (!part.empty() && part != ".") {
            if (!current.empty() && current.back() != '/') current += '/';
            current += part;
            if (mkdir(current.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) break;
        offset = slash + 1;
    }
    return true;
}

int main(int argc, char *argv[]) {
    int exists = 0;
    const auto parsed = pm_tiny::parse_daemon_arguments(argc, argv, pm_tiny::daemon_platform::posix);
    if (!parsed.success) {
        std::fprintf(stderr, "pm_tiny: %s\n\n%s", parsed.error.c_str(),
                     pm_tiny::daemon_usage(argc > 0 ? argv[0] : "pm_tiny",
                                           pm_tiny::daemon_platform::posix).c_str());
        return 2;
    }
    if (parsed.options.help) {
        std::fputs(pm_tiny::daemon_usage(argc > 0 ? argv[0] : "pm_tiny",
                                         pm_tiny::daemon_platform::posix).c_str(), stdout);
        return EXIT_SUCCESS;
    }
    if (parsed.options.version) {
        std::printf("pm_tiny %s\n", PM_TINY_VERSION);
        return EXIT_SUCCESS;
    }
    const auto resolved = pm_tiny::resolve_daemon_config(parsed.options,
                                                         pm_tiny::daemon_platform::posix);
    if (!resolved.success) {
        std::fprintf(stderr, "pm_tiny: %s\n", resolved.error.c_str());
        return EXIT_FAILURE;
    }
    const auto &pm_tiny_cfg = resolved.config;
    std::string environment_error;
    const std::pair<const char *, std::string> exported[] = {
        {PM_TINY_HOME, pm_tiny_cfg.home_dir},
        {PM_TINY_LOG_FILE, pm_tiny_cfg.log_file},
        {PM_TINY_PROG_CFG_FILE, pm_tiny_cfg.program_config_file},
        {PM_TINY_APP_LOG_DIR, pm_tiny_cfg.app_log_dir},
        {PM_TINY_APP_ENVIRON_DIR, pm_tiny_cfg.app_environ_dir},
        {PM_TINY_SOCK_FILE, pm_tiny_cfg.socket_file},
        {PM_TINY_UDS_ABSTRACT_NAMESPACE, pm_tiny_cfg.uds_abstract_namespace ? "1" : "0"},
        {PM_TINY_PROCESS_TREE_MODE, pm_tiny_cfg.process_tree_mode},
        {PM_TINY_CGROUP_ROOT, pm_tiny_cfg.cgroup_root},
        {PM_TINY_LOG_LEVEL, pm_tiny_cfg.log_level},
        {PM_TINY_LOG_MAX_SIZE_KB, std::to_string(pm_tiny_cfg.log_max_size_kb)},
        {PM_TINY_LOG_ARCHIVE_COUNT, std::to_string(pm_tiny_cfg.log_archive_count)}
    };
    for (const auto &entry : exported) {
        if (!pm_tiny::set_daemon_environment(entry.first, entry.second, environment_error)) {
            std::fprintf(stderr, "pm_tiny: %s\n", environment_error.c_str());
            return EXIT_FAILURE;
        }
    }
    const std::string &pm_tiny_home_dir = pm_tiny_cfg.home_dir;
    const std::string &pm_tiny_lock_file = pm_tiny_cfg.lock_file;
    exists = pm_tiny::is_directory_exists(pm_tiny_home_dir.c_str());
    PM_TINY_DLOG_DEBUG("pm_tiny home:%s", pm_tiny_home_dir.c_str());
    if (exists == -1) {
        perror("is_directory_exists");
        exit(EXIT_FAILURE);
    }
    if (!exists) {
        if (!create_directory_tree(pm_tiny_home_dir)) {
            PM_TINY_DLOG_ERROR_ERRNO("mkdir %s", pm_tiny_home_dir.c_str());
            exit(EXIT_FAILURE);
        }
    }
    const std::string &pm_tiny_log_file = pm_tiny_cfg.log_file;
    const std::string &pm_tiny_prog_cfg_file = pm_tiny_cfg.program_config_file;
    const std::string &pm_tiny_app_log_dir = pm_tiny_cfg.app_log_dir;
    const std::string &pm_tiny_app_environ_dir = pm_tiny_cfg.app_environ_dir;
    auto mkdir_if_need = [](const std::string &dir) {
        int exists = pm_tiny::is_directory_exists(dir.c_str());
        if (exists == -1) {
            PM_TINY_DLOG_ERROR_ERRNO("is_directory_exists");
            exit(EXIT_FAILURE);
        }
        if (!exists) {
            if (!create_directory_tree(dir)) {
                PM_TINY_DLOG_ERROR_ERRNO("mkdir %s", dir.c_str());
                exit(EXIT_FAILURE);
            }
        }
    };
    mkdir_if_need(pm_tiny_app_log_dir);
    mkdir_if_need(pm_tiny_app_environ_dir);
    if (parsed.options.daemonize) {
        daemonize();
    }
    pm_tiny::daemon_log_level_t minimum_level;
    if (!pm_tiny::parse_daemon_log_level(pm_tiny_cfg.log_level, minimum_level)) {
        std::fprintf(stderr, "pm_tiny: invalid log level: %s\n", pm_tiny_cfg.log_level.c_str());
        return EXIT_FAILURE;
    }
    pm_tiny::daemon_log_config_t log_config;
    log_config.path = pm_tiny_log_file;
    log_config.max_size_bytes = static_cast<std::size_t>(pm_tiny_cfg.log_max_size_kb) * 1024U;
    log_config.archive_count = pm_tiny_cfg.log_archive_count;
    log_config.mirror_console = !parsed.options.daemonize;
    log_config.minimum_level = minimum_level;
    std::string log_error;
    pm_tiny::configure_daemon_log(log_config, log_error);
    int lock_fp = create_lock_pid_file(pm_tiny_lock_file.c_str());
    if (lock_fp < 0) {
        exit(EXIT_FAILURE);
    }
    pm_tiny_server_t pm_tiny_server;
    pm_tiny_server.effective_daemon_config = pm_tiny_cfg;
    pm_tiny_server.daemon_options = parsed.options;
    pm_tiny_server.started_monotonic_ms = pm_tiny::time::gettime_monotonic_ms();
    pm_tiny_server.pm_tiny_home_dir = pm_tiny_home_dir;
    pm_tiny_server.pm_tiny_prog_cfg_file = pm_tiny_prog_cfg_file;
    pm_tiny_server.pm_tiny_log_file = pm_tiny_log_file;
    pm_tiny_server.pm_tiny_app_log_dir = pm_tiny_app_log_dir;
    pm_tiny_server.pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
    pm_tiny_server.pm_tiny_sock_file = pm_tiny_cfg.socket_file;
    pm_tiny_server.uds_abstract_namespace = pm_tiny_cfg.uds_abstract_namespace;
    pm_tiny_server.allowed_uids = pm_tiny_cfg.allowed_uids;
    pm_tiny_server.allowed_gids = pm_tiny_cfg.allowed_gids;
    pm_tiny_server.pm_tiny_lock_file = pm_tiny_lock_file;
    if (!pm_tiny_server.init_process_tree(pm_tiny_cfg.process_tree_mode,
                                          pm_tiny_cfg.cgroup_root)) {
        delete_lock_pid_file(lock_fp, pm_tiny_lock_file.c_str());
        exit(EXIT_FAILURE);
    }
    try {
        pm_tiny_server.lmkdFd = pm_tiny::connect_lmkd();
    } catch (const std::exception &ex) {
        PM_TINY_DLOG_ERROR("connect lmkd error:%s", ex.what());
    }
    start(pm_tiny_server);
    delete_lock_pid_file(lock_fp, pm_tiny_lock_file.c_str());
    return 0;
}
