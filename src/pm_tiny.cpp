#include "pm_tiny_server.h"
#include "log.h"
#include "time_util.h"

using prog_ptr_t = pm_tiny::prog_ptr_t;
using proglist_t = pm_tiny::proglist_t;
using pm_tiny_server_t=pm_tiny::pm_tiny_server_t;


void kill_prog(proglist_t &progs);

size_t get_current_alive_prog(proglist_t &pm_tiny_progs);

void check_prog_has_event(int total_ready_fd, proglist_t &pm_tiny_progs,
                          fd_set &rfds, int &_readyfd);

void check_sock_has_event(int total_ready_fd,
                          pm_tiny_server_t &pm_tiny_server, int &_readyfd, fd_set &rfds, fd_set &wfds,
                          std::vector<pm_tiny::session_ptr_t> &sessions);

void check_listen_sock_has_event(int sock_fd,
                                 std::vector<pm_tiny::session_ptr_t> &sessions,
                                 int &_readyfd, fd_set &rfds);

pm_tiny::frame_ptr_t make_prog_info_data(proglist_t &pm_tiny_progs);

std::shared_ptr<pm_tiny::frame_t>
handle_cmd_start(pm_tiny_server_t &pm_tiny_server, pm_tiny::iframe_stream &ifs);

static sig_atomic_t exit_signal = 0;
static sig_atomic_t alarm_signal = 0;
static sig_atomic_t hup_signal = 0;
static sig_atomic_t exit_chld_signal = 0;

void sig_exit_handler(int sig, siginfo_t *info, void *ucontext) {
    pm_tiny::logger->safe_signal_log(sig);
    exit_signal = sig;
}

void sig_chld_handler(int sig, siginfo_t *info, void *ucontext) {
    pm_tiny::logger->safe_signal_log(sig);
    exit_chld_signal = sig;
}

void sig_alarm_handler(int sig, siginfo_t *info, void *ucontext) {
    pm_tiny::logger->safe_signal_log(sig);
    alarm_signal = sig;
}

void sig_hup_handler(int sig, siginfo_t *info, void *ucontext) {
    pm_tiny::logger->safe_signal_log(sig);
    hup_signal = sig;
}

pid_t wait_any_nohang(int *wstat) {
    return pm_tiny::safe_waitpid(-1, wstat, WNOHANG);
}


static void check_delayed_exit_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_exit_signal = exit_signal;
    exit_signal = 0;
    if (save_exit_signal) {
        bool terminate = save_exit_signal == SIGTERM
                         || save_exit_signal == SIGINT
                         || save_exit_signal == SIGSTOP;
        if (terminate) {
            kill_prog(pm_tiny_progs);
            tiny_server.server_exit = 1;
        }
    }
}


static void get_add_remove_progs(proglist_t &pm_tiny_progs, proglist_t &new_proglist,
                                 proglist_t &add_progs, proglist_t &remove_progs) {
    std::list<std::tuple<prog_ptr_t, prog_ptr_t>> equal_progs;
    for (auto iter = std::begin(pm_tiny_progs); iter != std::end(pm_tiny_progs); iter++) {
        for (auto n_iter = std::begin(new_proglist); n_iter != std::end(new_proglist); n_iter++) {
            if ((*n_iter)->name == (*iter)->name && (*n_iter)->work_dir == (*iter)->work_dir
                && (*n_iter)->args == (*iter)->args) {
                equal_progs.emplace_back(std::make_tuple(*iter, *n_iter));
                break;
            }
        }
    }
    for (auto n_iter = std::begin(new_proglist); n_iter != std::end(new_proglist); n_iter++) {
        bool find = false;
        for (auto iter = std::begin(equal_progs); iter != std::end(equal_progs); iter++) {
            if ((*n_iter) == std::get<1>(*iter)) {
                find = true;
                break;
            }
        }
        if (!find) {
            add_progs.emplace_back(*n_iter);
        }
    }

    for (auto iter = std::begin(pm_tiny_progs); iter != std::end(pm_tiny_progs); iter++) {
        bool find = false;
        for (auto eiter = std::begin(equal_progs); eiter != std::end(equal_progs); eiter++) {
            if (*iter == std::get<0>(*eiter)) {
                find = true;
                break;
            }
        }
        if (!find) {
            remove_progs.emplace_back(*iter);
        }
    }
}

auto xx_kill_1 = [](prog_ptr_t &p, int signo) {
    bool find = false;
    if (p->pid != -1) {
        find = true;
//        logger->debug("kill %s(%d)", p->name.c_str(), p->pid);
        int rt = kill(p->pid, signo);
        if (rt == -1) {
            pm_tiny::logger->syscall_errorlog("kill");
        }
    }
    return find;
};
auto xx_wait_1 = [](prog_ptr_t &p, int options) {
    bool find = false;
    if (p->pid != -1) {
        int rc = pm_tiny::safe_waitpid(p->pid, nullptr, options);
        if (rc == p->pid) {
//            logger->debug("waitpid %s(%d)", p->name.c_str(), p->pid);
            p->pid = -1;
            p->state = PM_TINY_PROG_STATE_STOPED;
        } else {
            find = true;
        }
    }
    return find;
};

void kill_prog(prog_ptr_t prog) {
    bool find = xx_kill_1(prog, SIGTERM);
    if (find) {
        pm_tiny::sleep_waitfor(prog->kill_timeout_sec, [&find, &prog]() {
            find = xx_wait_1(prog, WNOHANG);
            return !find;
        });
        if (find) {
            PM_TINY_LOG_I("force kill %s(pid:%d)", prog->name.c_str(), prog->pid);
            xx_kill_1(prog, SIGKILL);
            pm_tiny::sleep_waitfor(1, [&find, &prog]() {
                find = xx_wait_1(prog, 0);
                return !find;
            });
        }
    }
    prog->close_fds();
}

void kill_prog(proglist_t &progs) {
    if (progs.empty())return;
    auto xx_kill = [](proglist_t &progs, int signo) {
        bool find = false;
        for (auto iter = std::begin(progs); iter != std::end(progs); iter++) {
            auto p = *iter;
            auto f = xx_kill_1(p, signo);
            find = f || find;
        }
        return find;
    };
    auto xx_wait = [](proglist_t &progs, int options) {
        bool find = false;
        for (auto iter = std::begin(progs); iter != std::end(progs); iter++) {
            auto p = *iter;
            auto f = xx_wait_1(p, options);
            find = f || find;
        }
        return find;
    };
    bool find = xx_kill(progs, SIGTERM);
    if (find) {
        auto max_iter=std::max_element(progs.begin(), progs.end(),
                                       [](const prog_ptr_t &p1, const prog_ptr_t &p2) {
                                           return p1->kill_timeout_sec < p2->kill_timeout_sec;
                                       });
        auto kill_timeout_sec = (*max_iter)->kill_timeout_sec;
        pm_tiny::sleep_waitfor(kill_timeout_sec, [&]() {
            find = xx_wait(progs, WNOHANG);
            return !find;
        });
        if (find) {
            pm_tiny::logger->debug("force kill");
            xx_kill(progs, SIGKILL);
            pm_tiny::sleep_waitfor(1, [&]() {
                find = xx_wait(progs, 0);
                return !find;
            });
        }
    }
    std::for_each(std::begin(progs), std::end(progs),
                  [](prog_ptr_t &prog) {
                      prog->close_fds();
                      prog->set_state(PM_TINY_PROG_STATE_STOPED);
                  });
}

static void log_proglist(proglist_t &pm_tiny_progs) {
    std::stringstream ss;
    int i = 0;
    ss << '\n';
    using pm_tiny::operator<<;
    for (auto it = pm_tiny_progs.begin(); it != pm_tiny_progs.end(); it++) {
        ss << "--- " << i++ << " ---\n";
        ss << *((*it).get()) << "\n";
    }
    pm_tiny::logger->debug(ss.str().c_str());
}

/**
 * 检测是否收到过SIGHUP信号，如果存在重新加载配置
 * */
static void check_delayed_hup_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    if (tiny_server.server_exit)return;
    sig_atomic_t save_hup_signal = hup_signal;
    hup_signal = 0;
    if (save_hup_signal) {
        proglist_t new_proglist;
        tiny_server.parse_cfg(new_proglist);
        proglist_t add_progs;
        proglist_t remove_progs;
        get_add_remove_progs(pm_tiny_progs, new_proglist, add_progs, remove_progs);
        pm_tiny::logger->debug("prog add:%d remove:%d", add_progs.size(), remove_progs.size());
        kill_prog(remove_progs);
        pm_tiny::logger->debug("--- erase before ---");
        log_proglist(pm_tiny_progs);
        pm_tiny_progs.erase(
                std::remove_if(pm_tiny_progs.begin(), pm_tiny_progs.end(), [&remove_progs](const prog_ptr_t &prog) {
                    return std::find(remove_progs.begin(), remove_progs.end(), prog) != remove_progs.end();
                }), pm_tiny_progs.end());
        pm_tiny::logger->debug("--- erase after ---");
        log_proglist(pm_tiny_progs);
        tiny_server.restart_startfailed();
        std::for_each(std::begin(add_progs), std::end(add_progs), [&tiny_server](prog_ptr_t &prog) {
            int ret = tiny_server.spawn_prog(*prog);
            if (ret != -1) {
                prog->init_prog_log();
            }
        });
        std::copy(add_progs.begin(), add_progs.end(), std::back_inserter(pm_tiny_progs));
        pm_tiny::logger->debug("--- add after ---");
        log_proglist(pm_tiny_progs);
    }
}

void log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus) {
    const char *prog_name = "Unkown";
    float run_time = NAN;
    int restart_count = 0;
    if (prog) {
        prog_name = (char *) prog->name.c_str();
        run_time = (float) (pm_tiny::time::gettime_monotonic_ms() - prog->last_startup_ms) / (60 * 1000.0f);
        restart_count = prog->dead_count;
    }
    if (WIFEXITED(wstatus)) {
        int exit_code = WEXITSTATUS(wstatus);
        pm_tiny::logger->info("pid:%d name:%s exited, exit code %d run time:%.3f minutes restart:%d\n",
                     pid, prog_name, exit_code, run_time, restart_count);
    } else if (WIFSIGNALED(wstatus)) {
        int kill_signo = WTERMSIG(wstatus);
        char buf[80] = {0};
        mgr::utils::signal::signo_to_str(kill_signo, buf, false);
        pm_tiny::logger->info("pid:%d name:%s killed by signal %s run time:%.3f minutes restart:%d\n",
                     pid, prog_name, buf, run_time, restart_count);
    }
}

/*
 * 检查是否有子进程退出，如果有则回收子进程空间(waitpid)
 * */
void check_delayed_chld_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    int wstatus, w, rc;
    sig_atomic_t save_exit_chld_signal = exit_chld_signal;
    exit_chld_signal = 0;
    if (save_exit_chld_signal) {
        size_t wait_proc_num = get_current_alive_prog(pm_tiny_progs);
        while (wait_proc_num > 0) {
            rc = wait_any_nohang(&wstatus);
            if (rc > 0) {
                auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                         [&rc](const prog_ptr_t &v) { return v->pid == rc; });

                if (iter != pm_tiny_progs.end()) {
                    auto p = *iter;
                    log_proc_exit_status(&(*p), rc, wstatus);
                    auto now_ms = pm_tiny::time::gettime_monotonic_ms();
                    auto life_time = now_ms - p->last_startup_ms;
                    p->last_wstatus = wstatus;
                    if ((now_ms - p->last_dead_time_ms) > p->moniter_duration_threshold) {
                        p->last_dead_time_ms = now_ms;
                        p->dead_count_timer = 0;
                    } else {
                        p->dead_count_timer++;
                    }
                    p->close_fds();
                    bool normal_exit = ((WIFSIGNALED(wstatus) && (WTERMSIG(wstatus) == p->pendding_signal))
                                        || (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0));
                    p->pendding_signal = 0;
                    if (!normal_exit) {
                        if ((p->dead_count_timer < p->moniter_duration_max_dead_count ||
                             p->moniter_duration_max_dead_count <= 0) &&
                            life_time > p->min_lifetime_threshold) {
                            p->dead_count++;
                            if (!tiny_server.server_exit) {
                                int ret = tiny_server.start_prog(p);
                                if (ret == -1) {
                                    pm_tiny::logger->syscall_errorlog("chld_sig try restart `%s` fail",
                                                                      p->name.c_str());
                                }
                            }
                        } else {
                            if (life_time > p->min_lifetime_threshold) {
                                pm_tiny::logger->info("%s die %d times within %.4f minutes\n ", p->name.c_str(),
                                             p->dead_count_timer, p->moniter_duration_threshold / (60 * 1000.0f));
                            } else {
                                pm_tiny::logger->info("life time %.4fs <= %.4fs\n", life_time / 1000.0f,
                                             p->min_lifetime_threshold / 1000.0f);
                            }
                            p->set_state(PM_TINY_PROG_STATE_STOPED);
                        }
                    } else {
                        p->set_state(PM_TINY_PROG_STATE_EXIT);
                    }
                } else {
                    log_proc_exit_status(nullptr, rc, wstatus);
                }

                size_t proc_num = get_current_alive_prog(pm_tiny_progs);
                pm_tiny::logger->info("current proc:%zu\n", proc_num);
                if (proc_num <= 0) {
                    break;
                }
            } else {
                if (rc == -1) {
                    pm_tiny::logger->syscall_errorlog("waitpid");
                }
                break;
            }
        }
    }
}

size_t get_current_alive_prog(proglist_t &pm_tiny_progs) {
    size_t wait_proc_num = std::count_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                         [](const prog_ptr_t &v) {
                                             return v->pid != -1;
                                         });
    return wait_proc_num;
}

void check_delayed_sigs(pm_tiny_server_t &tiny_server) {
    check_delayed_exit_sig(tiny_server);
    check_delayed_hup_sig(tiny_server);
    check_delayed_chld_sig(tiny_server);
}

void install_exit_handler() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_exit_handler;
    sigaction(SIGTERM, &act, nullptr);
    sigaction(SIGINT, &act, nullptr);
    sigaction(SIGSTOP, &act, nullptr);
}

void install_chld_handler() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_chld_handler;
    sigaction(SIGCHLD, &act, nullptr);
}

void install_hup_handler() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_hup_handler;
    sigaction(SIGHUP, &act, nullptr);
}

int open_uds_listen_fd(const std::string& sock_path) {
    int sfd;
    struct sockaddr_un my_addr;
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        pm_tiny::logger->syscall_errorlog("socket");
        exit(EXIT_FAILURE);
    };

    memset(&my_addr, 0, sizeof(struct sockaddr_un));
    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, sock_path.c_str(),
            sizeof(my_addr.sun_path) - 1);

    int rc = pm_tiny::set_nonblock(sfd);
    if (rc < 0) {
        pm_tiny::logger->syscall_errorlog("fcntl");
        exit(EXIT_FAILURE);
    }

    if (bind(sfd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) == -1) {
        pm_tiny::logger->syscall_errorlog("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(sfd, 5) == -1) {
        pm_tiny::logger->syscall_errorlog("listen");
        exit(EXIT_FAILURE);
    }
    return sfd;
}


void start(pm_tiny_server_t &pm_tiny_server) {
    pm_tiny::logger->debug("pm_tiny pid:%d\n", getpid());
    fd_set rfds;
    fd_set wfds;
    int rc = 0;
    auto sock_path = pm_tiny_server.pm_tiny_home_dir + "/pm_tinyd.sock";
    unlink(sock_path.c_str());

    rc = pm_tiny::set_sigaction(SIGPIPE, SIG_IGN);
    if (rc == -1) {
        pm_tiny::logger->syscall_errorlog("sigaction SIGPIPE");
    }
    int sock_fd = open_uds_listen_fd(sock_path);
    pm_tiny_server.parse_cfg();
    if (pm_tiny_server.pm_tiny_progs.empty()) {
        pm_tiny::logger->debug("progs empty\n");
    }

    install_exit_handler();
    install_chld_handler();
    install_hup_handler();
    pm_tiny_server.spawn();
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    std::vector<pm_tiny::session_ptr_t> sessions;
    using pm_tiny::operator<<;
    sigset_t smask, empty_mask,osigmask;
    sigemptyset(&smask);
    sigaddset(&smask, SIGSTOP);
    sigaddset(&smask, SIGTERM);
    sigaddset(&smask, SIGINT);
    sigaddset(&smask, SIGCHLD);
    sigaddset(&smask, SIGHUP);
    rc = sigprocmask(SIG_SETMASK, nullptr, &osigmask);
    if (rc == -1) {
        PM_TINY_LOG_E_SYS("sigprocmask");
        exit(EXIT_FAILURE);
    }
    sigemptyset(&empty_mask);

    while (true) {
        if (sigprocmask(SIG_BLOCK, &smask, nullptr) == -1) {
            PM_TINY_LOG_E_SYS("sigprocmask");
            exit(EXIT_FAILURE);
        }
        check_delayed_sigs(pm_tiny_server);
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int max_fd = -1;
        auto f_fd2set = [&max_fd](const int &fd, fd_set &fds) {
            if (fd < 0)return;
            FD_SET(fd, &fds);
            if (fd > max_fd) {
                max_fd = fd;
            }
        };
        auto f_fd2rfds = [&f_fd2set, &rfds](const int &fd) {
            f_fd2set(fd, rfds);
        };
        auto f_fd2wfds = [&f_fd2set, &wfds](const int &fd) {
            f_fd2set(fd, wfds);
        };
        for (auto &prog_info: pm_tiny_progs) {
            std::for_each(std::begin(prog_info->rpipefd),
                          std::end(prog_info->rpipefd), f_fd2rfds);
        }
        if (!pm_tiny_server.server_exit) {
            std::for_each(sessions.begin(), sessions.end(),
                          [&f_fd2rfds, &f_fd2wfds](const pm_tiny::session_ptr_t &session) {

                              f_fd2rfds(session->get_fd());
                              if (session->sbuf_size() > 0) {
                                  printf("%d sbuf_size:%d\n", session->get_fd(), session->sbuf_size());
                                  f_fd2wfds(session->get_fd());
                              }
                          });
            f_fd2rfds(sock_fd);
        } else if (!sessions.empty()) {
            close(sock_fd);
            std::for_each(sessions.begin(), sessions.end(),
                          [](const pm_tiny::session_ptr_t &session) {
                              session->close();
                          });
            sessions.clear();
        }
        if (max_fd < 0) {
            if (!pm_tiny_server.server_exit) {
                pselect(0, nullptr, nullptr, nullptr, nullptr,&empty_mask);
                continue;
            } else {
                break;
            }
        }
        rc = pselect(max_fd + 1, &rfds, &wfds, nullptr, nullptr,&empty_mask);
        if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
            PM_TINY_LOG_E_SYS("sigprocmask");
            exit(EXIT_FAILURE);
        }
        if (rc == -1) {
            if (errno == EINTR) {
                continue;
            }
            pm_tiny::logger->syscall_errorlog("select");
            break;
        }
        int _readyfd = 0;
        check_prog_has_event(rc, pm_tiny_progs, rfds, _readyfd);
        if (_readyfd < rc && !pm_tiny_server.server_exit) {
            check_listen_sock_has_event(sock_fd, sessions, _readyfd, rfds);
            if (_readyfd < rc) {
                check_sock_has_event(rc, pm_tiny_server, _readyfd, rfds, wfds,
                                     sessions);
            }
        }
    }
    if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
        PM_TINY_LOG_E_SYS("sigprocmask");
        exit(EXIT_FAILURE);
    }
    pm_tiny_server.server_exit = 1;
    kill_prog(pm_tiny_progs);
    pm_tiny_server.close_fds();
    unlink(sock_path.c_str());
    pm_tiny::logger->info("pm_tiny exit");
}

void check_sock_has_event(int total_ready_fd,
                          pm_tiny_server_t &pm_tiny_server, int &_readyfd,
                          fd_set &rfds, fd_set &wfds,
                          std::vector<pm_tiny::session_ptr_t> &sessions) {
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    bool closed_session = false;
    for (int i = 0; i < sessions.size() && _readyfd < total_ready_fd; i++) {
        std::shared_ptr<pm_tiny::session_t> &session = sessions[i];
        int s_fd = session->get_fd();
        if (FD_ISSET(s_fd, &rfds)) {
            auto rf = session->read_frame();
            if (!session->is_close()) {
                pm_tiny::logger->debug("%d is readable\n", s_fd);
            }
            if (rf) {
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
                    auto wf = std::make_shared<pm_tiny::frame_t>();
                    if (iter == pm_tiny_progs.end()) {
                        pm_tiny::fappend_value<int>(*wf, 0x1);
                        pm_tiny::fappend_value(*wf, "not found `" + name + "`");
                    } else {
                        if ((*iter)->pid != -1) {
                            kill_prog(*iter);
                            pm_tiny::fappend_value<int>(*wf, 0);
                            pm_tiny::fappend_value(*wf, "success");
                        } else {
                            pm_tiny::fappend_value<int>(*wf, 2);
                            pm_tiny::fappend_value(*wf, "`" + name + "` not running");
                        }
                    }
                    session->write_frame(wf);
                } else if (f_type == 0x25) {//start
                    std::shared_ptr<pm_tiny::frame_t> wf = handle_cmd_start(pm_tiny_server, ifs);
                    session->write_frame(wf);
                } else if (f_type == 0x26) {//save
                    auto wf = std::make_shared<pm_tiny::frame_t>();
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
                    auto wf = std::make_shared<pm_tiny::frame_t>();
                    if (iter == pm_tiny_progs.end()) {
                        pm_tiny::fappend_value<int>(*wf, 0x1);
                        pm_tiny::fappend_value(*wf, "not found `" + name + "`");
                    } else {
                        if ((*iter)->pid != -1) {
                            kill_prog(*iter);
                        }
                        pm_tiny_progs.erase(iter);
                        pm_tiny::fappend_value<int>(*wf, 0);
                        pm_tiny::fappend_value(*wf, "success");
                    }
                    session->write_frame(wf);
                } else if (f_type == 0x28) {//restart
                    std::string name;
                    ifs >> name;
                    auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                             [&name](const prog_ptr_t &prog) {
                                                 return prog->name == name;
                                             });
                    auto wf = std::make_shared<pm_tiny::frame_t>();
                    if (iter == pm_tiny_progs.end()) {
                        pm_tiny::fappend_value<int>(*wf, 0x1);
                        pm_tiny::fappend_value(*wf, "not found `" + name + "`");
                    } else {
                        bool is_alive = (*iter)->pid != -1;
//                        pm_tiny::time::CElapsedTimer elapsedTimer;
                        if (is_alive) {
                            kill_prog(*iter);
//                            PM_TINY_LOG_I("kill %s cost:%dms", (*iter)->name.c_str(),elapsedTimer.ms());
                        }
                        if (is_alive) {
                            (*iter)->dead_count++;
                        }
                        int rc = pm_tiny_server.start_prog(*iter);
                        if (rc == -1) {
                            std::string errmsg(strerror(errno));
                            pm_tiny::fappend_value<int>(*wf, 1);
                            pm_tiny::fappend_value(*wf, errmsg);
                        } else {
                            pm_tiny::fappend_value<int>(*wf, 0);
                            pm_tiny::fappend_value(*wf, "success");
                        }
//                        PM_TINY_LOG_I("restart %s cost:%dms", (*iter)->name.c_str(),elapsedTimer.ms());
                    }
                    session->write_frame(wf);
                }else if(f_type==0x29){
                    auto wf = std::make_shared<pm_tiny::frame_t>();
                    pm_tiny::fappend_value(*wf, PM_TINY_VERSION);
                    session->write_frame(wf);
                } else {
                    pm_tiny::logger->info("unkown framae type:%#02X", f_type);
                }
            } else {
                //ignore
            }
            if (session->is_close()) {
                closed_session = true;
                pm_tiny::logger->debug("%d is closed\n", s_fd);
            }
            _readyfd++;
        }
        if (!session->is_close() && FD_ISSET(s_fd, &wfds)) {
            session->write();
            _readyfd++;
        }
    }
    if (closed_session) {
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                      [](const pm_tiny::session_ptr_t &session) {
                                          return session->is_close();
                                      }), sessions.end());
    }
}

std::shared_ptr<pm_tiny::frame_t> handle_cmd_start(pm_tiny_server_t &pm_tiny_server,
                                                   pm_tiny::iframe_stream &ifs) {
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    //name:cwd:command
    std::string name;
    std::string cwd;
    std::string command;
    int local_resolved;
    int env_num;
    int kill_timeout;
    ifs >> name >> cwd >> command >> local_resolved >> env_num;
    std::vector<std::string> envs;
    envs.resize(env_num);
    for (int k = 0; k < env_num; k++) {
        ifs >> envs[k];
    }
    ifs >> kill_timeout;
//   std::cout << "name:`" + name << "` cwd:`" << cwd << "` command:`" << command
//   << "` local_resolved:" << local_resolved << std::endl;
    auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                             [&name](const prog_ptr_t &prog) {
                                 return prog->name == name;
                             });
    auto wf = std::make_shared<pm_tiny::frame_t>();
    if (iter == pm_tiny_progs.end()) {
        const prog_ptr_t prog = pm_tiny_server.create_prog(name, cwd, command, envs,kill_timeout);
        if (!prog) {
            pm_tiny::fappend_value<int>(*wf, 0x3);
            pm_tiny::fappend_value(*wf, "create `" + name + "` fail");
        } else {
            int ret = pm_tiny_server.start_and_add_prog(prog);
            if (ret == -1) {
                std::string errmsg(strerror(errno));
                pm_tiny::fappend_value<int>(*wf, 1);
                pm_tiny::fappend_value(*wf, errmsg);
            } else {
                pm_tiny::fappend_value<int>(*wf, 0);
                pm_tiny::fappend_value(*wf, "success");
            }
        }
    } else {
        prog_ptr_t _p = *iter;
        if (_p->pid == -1) {
            const prog_ptr_t prog = pm_tiny_server.create_prog(name, cwd, command, envs,kill_timeout);
            if (local_resolved && prog
                && (prog->work_dir != _p->work_dir || prog->args != _p->args)) {
                pm_tiny::fappend_value<int>(*wf, 4);
                pm_tiny::fappend_value(*wf, "The cwd or command or environ has changed,"
                                            " please run the delete operation first");
            } else {
                _p->envs = envs;
                int rc = pm_tiny_server.start_prog(_p);
                if (rc == -1) {
                    std::string errmsg(strerror(errno));
                    pm_tiny::fappend_value<int>(*wf, 1);
                    pm_tiny::fappend_value(*wf, errmsg);
                } else {
                    pm_tiny::fappend_value<int>(*wf, 0);
                    pm_tiny::fappend_value(*wf, "success");
                }
            }
        } else {
            pm_tiny::fappend_value<int>(*wf, 2);
            pm_tiny::fappend_value(*wf, "`" + name + "` already running");
        }
    }
    return wf;
}

pm_tiny::frame_ptr_t make_prog_info_data(proglist_t &pm_tiny_progs) {
    //n pid:name:workdir:command:restart_count:state:VmRSSkiB
    auto f = std::make_shared<pm_tiny::frame_t>();
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
        pm_tiny::fappend_value(*f,VmRSSkiB);
    }
    return f;
}

void check_listen_sock_has_event(int sock_fd,
                                 std::vector<pm_tiny::session_ptr_t> &sessions,
                                 int &_readyfd, fd_set &rfds) {
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size = sizeof(struct sockaddr_un);
    if (FD_ISSET(sock_fd, &rfds)) {
        cfd = accept4(sock_fd, (struct sockaddr *) &peer_addr,
                      &peer_addr_size, SOCK_NONBLOCK);
        pm_tiny::logger->debug("accept fd:%d\n", cfd);
        sessions.emplace_back(std::make_shared<pm_tiny::session_t>(cfd, 0));
        _readyfd++;
    }
}

void check_prog_has_event(int total_ready_fd, proglist_t &pm_tiny_progs,
                          fd_set &rfds, int &_readyfd) {
    for (auto &prog_info: pm_tiny_progs) {
        if (_readyfd >= total_ready_fd) {
            break;
        }
        for (int i = 0; i < 2; i++) {
            int &fd = prog_info->rpipefd[i];
            if (fd < 0)continue;
            if (FD_ISSET(fd, &rfds)) {
                prog_info->read_pipe(i);
                _readyfd++;
            }
        }
    }
}


int create_lock_pid_file(const char *filepath) {
    char str[20];
    int lfp = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0640);
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
    write(lfp, str, strlen(str)); /* record pid to lockfile */
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
    for (i = getdtablesize(); i >= 0; --i) close(i); /* close all descriptors */
    i = open("/dev/null", O_RDWR);
    dup(i);
    dup(i); /* handle standart I/O */
    umask(027); /* set newly created file permissions */
    chdir("/");
}

struct command_args {
    int daemon = 0;
    std::string cfg_file;
    std::string home_dir;
};

int parse_command_args(int argc, char **argv,
                       struct command_args &args) {
    int index;
    int c;
    opterr = 0;
    while ((c = getopt(argc, argv, "dc:e:")) != -1)
        switch (c) {
            case 'd':
                args.daemon = 1;
                break;
            case 'c':
                args.cfg_file = optarg;
                break;
            case 'e':
                args.home_dir = optarg;
                break;
            case '?':
                if (isprint(optopt))
                    fprintf(stderr, "Unknown option `%c`.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x`.\n", optopt);
                return 1;
            default:
                exit(EXIT_FAILURE);
        }

    for (index = optind; index < argc; index++)
        fprintf(stderr, "Non-option argument %s\n", argv[index]);
    return 0;
}


int main(int argc, char *argv[]) {
    pm_tiny::logger = std::make_shared<pm_tiny::logger_t>();
    command_args args;
    int rc = 0;
    int exists = 0;
    parse_command_args(argc, argv, args);
    char cfg_path[PATH_MAX] = {0};
    char home_path[PATH_MAX] = {0};
    if (!args.cfg_file.empty()) {
        if (realpath(args.cfg_file.c_str(), cfg_path) == NULL) {
            PM_TINY_LOG_E_SYS("%s realpath", args.cfg_file.c_str());
            exit(EXIT_FAILURE);
        }
    }
    if (!args.home_dir.empty()) {
        if (realpath(args.home_dir.c_str(), home_path) == NULL) {
            PM_TINY_LOG_E_SYS("%s realpath", args.home_dir.c_str());
            exit(EXIT_FAILURE);
        }
    }
    std::string pm_tiny_home_dir = pm_tiny::get_pm_tiny_home_dir(home_path);
    std::string pm_lock_file = pm_tiny_home_dir + "/" + "pm_tiny.pid";
    exists = pm_tiny::is_directory_exists(pm_tiny_home_dir.c_str());
    PM_TINY_LOG_D("pm_tiny home:%s", pm_tiny_home_dir.c_str());
    if (exists == -1) {
        perror("is_directory_exists");
        exit(EXIT_FAILURE);
    }
    if (!exists) {
        rc = mkdir(pm_tiny_home_dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (rc == -1) {
            PM_TINY_LOG_E_SYS("mkdir %s", pm_tiny_home_dir.c_str());
            exit(EXIT_FAILURE);
        }
    }
    std::string pm_tiny_log_file = pm_tiny_home_dir + "/pm_tiny.log";
    std::string pm_tiny_cfg_file = cfg_path;
    std::string pm_tiny_app_log_dir = pm_tiny_home_dir + "/logs";
    std::string pm_tiny_app_environ_dir = pm_tiny_home_dir + "/environ";
    auto mkdir_if_need = [](const std::string &dir) {
        int exists = pm_tiny::is_directory_exists(dir.c_str());
        if (exists == -1) {
            pm_tiny::logger->syscall_errorlog("is_directory_exists");
            exit(EXIT_FAILURE);
        }
        if (!exists) {
            int rc = mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            if (rc == -1) {
                pm_tiny::logger->syscall_errorlog("mkdir %s", dir.c_str());
                exit(EXIT_FAILURE);
            }
        }
    };
    mkdir_if_need(pm_tiny_app_log_dir);
    mkdir_if_need(pm_tiny_app_environ_dir);
    if (pm_tiny_cfg_file.empty()) {
        pm_tiny_cfg_file = pm_tiny_home_dir + "/prog.cfg";
    }
    setenv("PM_TINY_HOME", pm_tiny_home_dir.c_str(), 0);
    setenv("PM_TINY_LOG_FILE", pm_tiny_log_file.c_str(), 1);
    setenv("PM_TINY_APP_LOG_DIR", pm_tiny_app_log_dir.c_str(), 1);
    setenv("PM_TINY_CFG_FILE", pm_tiny_cfg_file.c_str(), 1);
    if (args.daemon) {
        daemonize();
    }
    pm_tiny::logger = std::make_shared<pm_tiny::logger_t>(pm_tiny_log_file.c_str());
    int lock_fp = create_lock_pid_file(pm_lock_file.c_str());
    if (lock_fp < 0) {
        exit(EXIT_FAILURE);
    }
    pm_tiny_server_t pm_tiny_server;
    pm_tiny_server.pm_tiny_home_dir = pm_tiny_home_dir;
    pm_tiny_server.pm_tiny_cfg_file = pm_tiny_cfg_file;
    pm_tiny_server.pm_tiny_log_file = pm_tiny_log_file;
    pm_tiny_server.pm_tiny_app_log_dir = pm_tiny_app_log_dir;
    pm_tiny_server.pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
    start(pm_tiny_server);
    delete_lock_pid_file(lock_fp, pm_lock_file.c_str());
    return 0;
}