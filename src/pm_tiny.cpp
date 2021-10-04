#include <unistd.h>
#include "pm_tiny_server.h"
#include "log.h"
#include "time_util.h"
#include "prog.h"
#include "assert.h"
#include "ScopeGuard.h"
#include "pm_tiny_funcs.h"
#include "pm_sys.h"
#include "android_lmkd.h"

using prog_ptr_t = pm_tiny::prog_ptr_t;
using proglist_t = pm_tiny::proglist_t;
using pm_tiny_server_t = pm_tiny::pm_tiny_server_t;

size_t get_living_processes_count(proglist_t &pm_tiny_progs);

void check_prog_has_event(int total_ready_fd, proglist_t &pm_tiny_progs,
                          fd_set &rfds, int &_readyfd);

void check_sock_has_event(int total_ready_fd,
                          pm_tiny_server_t &pm_tiny_server, int &_readyfd, fd_set &rfds, fd_set &wfds,
                          std::vector<pm_tiny::session_ptr_t> &sessions);

void check_listen_sock_has_event(int sock_fd,
                                 std::vector<pm_tiny::session_ptr_t> &sessions,
                                 int &_readyfd, fd_set &rfds);

void remove_closed_session(std::vector<pm_tiny::session_ptr_t> &sessions);

static sig_atomic_t exit_signal = 0;
static sig_atomic_t alarm_signal = 0;
static sig_atomic_t hup_signal = 0;
static sig_atomic_t exit_chld_signal = 0;

void sig_exit_handler(int sig, siginfo_t *, void *) {
    pm_tiny::logger->safe_signal_log(sig);
    exit_signal = sig;
}

void sig_chld_handler(int sig, siginfo_t *, void *) {
    pm_tiny::logger->safe_signal_log(sig);
    exit_chld_signal = sig;
}

void sig_alarm_handler(int sig, siginfo_t *, void *) {
    pm_tiny::logger->safe_signal_log(sig);
    alarm_signal = sig;
}

void sig_hup_handler(int sig, siginfo_t *, void *) {
    pm_tiny::logger->safe_signal_log(sig);
    hup_signal = sig;
}

pid_t wait_any_nohang(int *wstat) {
    return pm_tiny::safe_waitpid(-1, wstat, WNOHANG);
}


static void check_delayed_exit_sig(pm_tiny_server_t &tiny_server) {
//    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_exit_signal = exit_signal;
    exit_signal = 0;
    if (save_exit_signal) {
        bool terminate = save_exit_signal == SIGTERM
                         || save_exit_signal == SIGINT
                         || save_exit_signal == SIGSTOP;
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
            PM_TINY_LOG_I("force kill %s(pid:%d)", prog->name.c_str(), prog->pid);
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
    sig_atomic_t save_exit_chld_signal = exit_chld_signal;
    exit_chld_signal = 0;
    if (save_exit_chld_signal) {
        proglist_t starting_prog;
        proglist_t penddingtask_progs;
        proglist_t deleting_progs;
        size_t wait_proc_num = get_living_processes_count(pm_tiny_progs);
        while (wait_proc_num > 0) {
            rc = wait_any_nohang(&wstatus);
            if (rc > 0) {
                auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                                         [rc](const prog_ptr_t &v) { return v->pid == rc; });

                if (iter != pm_tiny_progs.end()) {
                    auto p = *iter;
                    std::string exit_info = pm_tiny::prog_info_t::log_proc_exit_status(&(*p), rc, wstatus);
                    PM_TINY_LOG_I("%s", exit_info.c_str());
                    auto now_ms = p->update_count_timer();
                    auto life_time = now_ms - p->last_startup_ms;
                    p->last_wstatus = wstatus;
                    p->close_fds(tiny_server.lmkdFd);
                    if (p->state == PM_TINY_PROG_STATE_REQUEST_STOP ||
                        p->state == PM_TINY_PROG_STATE_REQUEST_DELETE) {
                        penddingtask_progs.push_back(p);
                    }
                    if (p->state == PM_TINY_PROG_STATE_REQUEST_DELETE) {
                        deleting_progs.push_back(p);
                    }
                    bool normal_exit = p->state == PM_TINY_PROG_STATE_REQUEST_STOP
                                       || p->state == PM_TINY_PROG_STATE_REQUEST_DELETE
                                       || !p->daemon;
                    if (!normal_exit) {
                        if (!p->is_reach_max_num_death() && life_time > p->min_lifetime_threshold) {
                            p->dead_count++;
                            int ret = tiny_server.start_prog(p);
                            if (ret == -1) {
                                PM_TINY_LOG_E_SYS("chld_sig try restart `%s` fail", p->name.c_str());
                                tiny_server.flag_startup_fail(p);
                            }
                            //Programs that depend on this program will not start
                        } else {
                            if (p->state == PM_TINY_PROG_STATE_STARTING) {
                                starting_prog.push_back(p);
                            }
                            if (life_time > p->min_lifetime_threshold) {
                                PM_TINY_LOG_I("`%s` die %d times within %.4fmin", p->name.c_str(),
                                              p->dead_count_timer,
                                              p->moniter_duration_threshold / (60 * 1000.0f));
                            } else {
                                PM_TINY_LOG_I("life time %.4fs <= %.4fs", life_time / 1000.0f,
                                              p->min_lifetime_threshold / 1000.0f);
                            }
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
                    std::string exit_info = pm_tiny::prog_info_t::log_proc_exit_status(nullptr, rc, wstatus);
                    PM_TINY_LOG_I("%s", exit_info.c_str());
                }

                size_t proc_num = get_living_processes_count(pm_tiny_progs);
//                PM_TINY_LOG_D("current proc:%zu\n", proc_num);
                if (proc_num <= 0) {
                    break;
                }
            } else {
                if (rc == -1) {
                    PM_TINY_LOG_E_SYS("waitpid");
                }
                break;
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
        if (prog->state == PM_TINY_PROG_STATE_REQUEST_STOP
            || prog->state == PM_TINY_PROG_STATE_REQUEST_DELETE) {
            if (prog->pid != -1 && prog->is_kill_timeout()) {
                prog->async_force_kill();
            }
        } else {
            if (tiny_server.is_exiting()) {
                continue;
            }
            if (prog->state == PM_TINY_PROG_STATE_STARTING) {
                if (prog->pid != -1 && prog->is_start_timeout()) {
                    if (prog->failure_action == pm_tiny::failure_action_t::SKIP
                        || prog->start_timeout == 0) {
                        starting_prog.push_back(prog);
                        prog->state = PM_TINY_PROG_STATE_RUNING;
                        prog->last_tick_timepoint = pm_tiny::time::gettime_monotonic_ms();
                        PM_TINY_LOG_D("start timeout:%s", prog->name.c_str());
                    } else if (prog->failure_action == pm_tiny::failure_action_t::RESTART) {
                        prog->async_kill_prog();
                        auto start_prog_task =
                                [&prog](pm_tiny_server_t &pm_tiny_server) {
                                    assert(prog->state != PM_TINY_PROG_STATE_RUNING);
                                    int rc = pm_tiny_server.start_prog(prog);
                                    if (rc == -1) {
                                        std::string errmsg(strerror(errno));
                                        PM_TINY_LOG_I("start prog fail:%s", errmsg.c_str());
                                        pm_tiny_server.flag_startup_fail(prog);
//                                    proglist_t pl;
//                                    pl.push_back(prog);
//                                    pm_tiny_server.spawn1(pl);
                                    } else {
                                        prog->dead_count++;
                                    }
                                };
                        prog->kill_pendingtasks.emplace_back(start_prog_task);
                    } else {
                        PM_TINY_LOG_I("`%s` start timeout reboot now.", prog->name.c_str());
                        pm_tiny::process_reboot();
                        reboot = true;
                        break;
                    }
                }
            } else if (prog->state == PM_TINY_PROG_STATE_RUNING) {
                if (prog->pid != -1 && prog->is_tick_timeout()) {
                    if (prog->failure_action == pm_tiny::failure_action_t::RESTART) {
                        PM_TINY_LOG_I("`%s` tick timeout restart now.", prog->name.c_str());
                        prog->async_kill_prog();
                        auto start_prog_task =
                                [&prog](pm_tiny_server_t &pm_tiny_server) {
                                    assert(prog->state != PM_TINY_PROG_STATE_RUNING);
                                    int rc = pm_tiny_server.start_prog(prog);
                                    if (rc == -1) {
                                        std::string errmsg(strerror(errno));
                                        PM_TINY_LOG_I("start prog fail:%s", errmsg.c_str());
//                                proglist_t pl;
//                                pl.push_back(prog);
//                                pm_tiny_server.spawn1(pl);
                                        pm_tiny_server.flag_startup_fail(prog);
                                    } else {
                                        prog->dead_count++;
                                    }
                                };
                        prog->kill_pendingtasks.emplace_back(start_prog_task);
                    } else if (prog->failure_action == pm_tiny::failure_action_t::REBOOT) {
                        PM_TINY_LOG_I("`%s` tick timeout reboot now.", prog->name.c_str());
                        pm_tiny::process_reboot();
                        reboot = true;
                        break;
                    } else if (prog->failure_action == pm_tiny::failure_action_t::SKIP) {
//                    PM_TINY_LOG_I("`%s` tick timeout skip.", prog->name.c_str());
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
                                             return v->pid != -1;
                                         });
    return wait_proc_num;
}

void check_delayed_sigs(pm_tiny_server_t &tiny_server) {
    check_delayed_exit_sig(tiny_server);
    check_delayed_chld_sig(tiny_server);
}

void install_exit_handler() {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_exit_handler;
    sigaction(SIGTERM, &act, nullptr);
    sigaction(SIGINT, &act, nullptr);
    sigaction(SIGSTOP, &act, nullptr);
}

void install_chld_handler() {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_chld_handler;
    sigaction(SIGCHLD, &act, nullptr);
}

void install_hup_handler() {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_hup_handler;
    sigaction(SIGHUP, &act, nullptr);
}

void install_alarm_handler() {
    struct sigaction act{};
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_alarm_handler;
    sigaction(SIGALRM, &act, nullptr);
}

int open_uds_listen_fd(const std::string &sock_path
                       ,bool enable_abstract_namespace) {
    int sfd;
    struct sockaddr_un my_addr{};
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        pm_tiny::logger->syscall_fatal("socket");
    };

    memset(&my_addr, 0, sizeof(struct sockaddr_un));
    my_addr.sun_family = AF_UNIX;
    socklen_t addr_length;
    if (enable_abstract_namespace) {
        my_addr.sun_path[0] = '\0';
        strncpy(my_addr.sun_path + 1, sock_path.c_str(), sizeof(my_addr.sun_path) - 2);
        addr_length = offsetof(struct sockaddr_un, sun_path) + sock_path.length() + 1;
    } else {
        strncpy(my_addr.sun_path, sock_path.c_str(),
                sizeof(my_addr.sun_path) - 1);
        addr_length = sizeof(struct sockaddr_un);
    }
    int rc = pm_tiny::set_nonblock(sfd);
    if (rc < 0) {
        pm_tiny::logger->syscall_fatal("fcntl");
    }
    rc = pm_tiny::set_cloexec(sfd);
    if (rc < 0) {
        pm_tiny::logger->syscall_fatal("set_cloexec");
    }
    if (bind(sfd, (struct sockaddr *) &my_addr,addr_length) == -1) {
        pm_tiny::logger->syscall_fatal("bind");
    }

    if (listen(sfd, 5) == -1) {
        pm_tiny::logger->syscall_fatal("listen");
    }
    return sfd;
}

int check_quit_or_reload(pm_tiny_server_t &pm_tiny_server) {
    auto pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    auto living_processes_count = get_living_processes_count(pm_tiny_progs);
    if (pm_tiny_server.is_exiting()) {
//        PM_TINY_LOG_D("==>prog_num:%d", living_processes_count);
        if (living_processes_count == 0) {
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
    PM_TINY_LOG_D("pm_tiny pid:%d\n", getpid());
    fd_set rfds;
    fd_set wfds;
    int rc = 0;
    auto sock_path = pm_tiny_server.pm_tiny_sock_file;
    if (!pm_tiny_server.uds_abstract_namespace) {
        unlink(sock_path.c_str());
    }
    rc = pm_tiny::set_sigaction(SIGPIPE, SIG_IGN);
    if (rc == -1) {
        PM_TINY_LOG_FATAL_SYS("sigaction SIGPIPE");
    }
    int sock_fd = open_uds_listen_fd(sock_path,
                                     pm_tiny_server.uds_abstract_namespace);
    rc = pm_tiny_server.parse_cfg();
    if (rc != 0) {
        exit(EXIT_FAILURE);
    }

    pm_tiny_server.show_prog_depends_info();

    install_exit_handler();
    install_chld_handler();
    install_hup_handler();
    install_alarm_handler();
    pm_tiny_server.spawn();
    std::vector<pm_tiny::session_ptr_t> &sessions = pm_tiny_server.sessions;
    sigset_t smask, empty_mask, osigmask;
    sigemptyset(&smask);
    sigaddset(&smask, SIGSTOP);
    sigaddset(&smask, SIGTERM);
    sigaddset(&smask, SIGINT);
    sigaddset(&smask, SIGCHLD);
    sigaddset(&smask, SIGHUP);
    sigaddset(&smask, SIGALRM);
    rc = sigprocmask(SIG_SETMASK, nullptr, &osigmask);
    if (rc == -1) {
        PM_TINY_LOG_FATAL_SYS("sigprocmask");
    }
    sigemptyset(&empty_mask);

    while (true) {
        if (sigprocmask(SIG_BLOCK, &smask, nullptr) == -1) {
            PM_TINY_LOG_FATAL_SYS("sigprocmask");
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
        proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
        for (auto &prog_info: pm_tiny_progs) {
            std::for_each(std::begin(prog_info->rpipefd),
                          std::end(prog_info->rpipefd), f_fd2rfds);
        }
        for (auto iter = sessions.begin(); iter != sessions.end();) {
            auto &session = *iter;
            if (session->sbuf_size() > 0) {
//              printf("%d sbuf_size:%d\n", session->get_fd(), session->sbuf_size());
                f_fd2wfds(session->get_fd());
            } else if (session->is_marked_close()) {
                session->close();
            }
            if (!session->is_close()) {
                f_fd2rfds(session->get_fd());
                iter++;
            } else {
                iter = sessions.erase(iter);
            }
        }
        f_fd2rfds(sock_fd);
        if (max_fd < 0) {
            pselect(0, nullptr, nullptr, nullptr, nullptr, &empty_mask);
            continue;
        }
        auto living_processes_count = get_living_processes_count(pm_tiny_progs);
        struct timespec timeout{};
        timeout.tv_nsec = 500 * 1e6;//500ms
        if (living_processes_count == 0 && pm_tiny_server.is_exiting()) {
            timeout.tv_nsec = 0;
        }
        rc = pselect(max_fd + 1, &rfds, &wfds, nullptr, &timeout, &empty_mask);
        int select_errno = errno;
        if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
            PM_TINY_LOG_FATAL_SYS("sigprocmask");
        }
        if (rc == 0) {
//            PM_TINY_LOG_D("timeout");
            auto ret = check_quit_or_reload(pm_tiny_server);
            if (ret) {
                break;
            }
            continue;
        }
        if (rc == -1) {
            if (select_errno == EINTR) {
                continue;
            }
            PM_TINY_LOG_E_SYS("select");
            break;
        }
        int _readyfd = 0;
        check_prog_has_event(rc, pm_tiny_progs, rfds, _readyfd);
        if (_readyfd < rc) {
            check_listen_sock_has_event(sock_fd, sessions, _readyfd, rfds);
            if (_readyfd < rc) {
                check_sock_has_event(rc, pm_tiny_server, _readyfd, rfds, wfds,
                                     sessions);
            }
        }
        remove_closed_session(sessions);
        auto ret = check_quit_or_reload(pm_tiny_server);
        if (ret) {
            break;
        }
    }
    if (sigprocmask(SIG_SETMASK, &osigmask, nullptr) == -1) {
        PM_TINY_LOG_FATAL_SYS("sigprocmask");
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
    PM_TINY_LOG_I("pm_tiny exit");
}

void mark_closed(pm_tiny::session_ptr_t &session) {
    if (session->shutdown_read() < 0) {
        PM_TINY_LOG_E_SYS("fd:%d", session->get_fd());
    }
}

void check_sock_has_event(int total_ready_fd,
                          pm_tiny_server_t &pm_tiny_server, int &_readyfd,
                          fd_set &rfds, fd_set &wfds,
                          std::vector<pm_tiny::session_ptr_t> &sessions) {
    for (int i = 0; i < static_cast<int>(sessions.size()) && _readyfd < total_ready_fd; i++) {
        auto &session = sessions[i];
        int s_fd = session->get_fd();
        if (FD_ISSET(s_fd, &rfds)) {
            auto rf = session->read_frame();
            if (rf) {
                try {
                    handle_frame(pm_tiny_server, rf, session);
                } catch (pm_tiny::BufferInsufficientException &ex) {
                    PM_TINY_LOG_E("fd:%d %s", s_fd, ex.what());
                    auto wf = std::make_unique<pm_tiny::frame_t>();
                    pm_tiny::fappend_value<int>(*wf, -0x1);
                    pm_tiny::fappend_value(*wf, "Invalid argument");
                    session->write_frame(wf);
                    mark_closed(session);
                }
            } else {
                //ignore
            }
            if (session->is_close()) {
                PM_TINY_LOG_D("%d is closed\n", s_fd);
            }
            _readyfd++;
        }
        if (!session->is_close() && FD_ISSET(s_fd, &wfds)) {
            session->write();
            _readyfd++;
        }
    }
}

void remove_closed_session(std::vector<pm_tiny::session_ptr_t> &sessions) {
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [](const pm_tiny::session_ptr_t &session) {
                                      return session->is_close();
                                  }), sessions.end());
}

void check_listen_sock_has_event(int sock_fd,
                                 std::vector<pm_tiny::session_ptr_t> &sessions,
                                 int &_readyfd, fd_set &rfds) {
    int cfd;
    struct sockaddr_un peer_addr{};
    socklen_t peer_addr_size = sizeof(struct sockaddr_un);
    if (FD_ISSET(sock_fd, &rfds)) {
        cfd = accept4(sock_fd, (struct sockaddr *) &peer_addr,
                      &peer_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
        PM_TINY_LOG_D("accept fd:%d\n", cfd);
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

struct command_args {
    int daemon = 0;
    std::string cfg_file;
};

int parse_command_args(int argc, char **argv,
                       struct command_args &args) {
    int index;
    int c;
    opterr = 0;
    while ((c = getopt(argc, argv, "dc:")) != -1)
        switch (c) {
            case 'd':
                args.daemon = 1;
                break;
            case 'c':
                args.cfg_file = optarg;
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
    pm_tiny::initialize();
    command_args args;
    int rc = 0;
    int exists = 0;
    parse_command_args(argc, argv, args);
    char cfg_path[PATH_MAX] = {0};
    if (!args.cfg_file.empty()) {
        if (realpath(args.cfg_file.c_str(), cfg_path) == nullptr) {
            PM_TINY_LOG_E_SYS("%s realpath", args.cfg_file.c_str());
            exit(EXIT_FAILURE);
        }
    }
    auto pm_tiny_cfg = pm_tiny::get_pm_tiny_config(cfg_path);
    std::string pm_tiny_home_dir = pm_tiny_cfg->pm_tiny_home_dir;
    std::string pm_tiny_lock_file = pm_tiny_cfg->pm_tiny_lock_file;
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
    std::string pm_tiny_log_file = pm_tiny_cfg->pm_tiny_log_file;
    std::string pm_tiny_prog_cfg_file = pm_tiny_cfg->pm_tiny_prog_cfg_file;
    std::string pm_tiny_app_log_dir = pm_tiny_cfg->pm_tiny_app_log_dir;
    std::string pm_tiny_app_environ_dir = pm_tiny_cfg->pm_tiny_app_environ_dir;
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
    if (args.daemon) {
        daemonize();
    }
    pm_tiny::logger = std::make_unique<pm_tiny::logger_t>(pm_tiny_log_file.c_str());
    int lock_fp = create_lock_pid_file(pm_tiny_lock_file.c_str());
    if (lock_fp < 0) {
        exit(EXIT_FAILURE);
    }
    pm_tiny_server_t pm_tiny_server;
    pm_tiny_server.pm_tiny_home_dir = pm_tiny_home_dir;
    pm_tiny_server.pm_tiny_prog_cfg_file = pm_tiny_prog_cfg_file;
    pm_tiny_server.pm_tiny_log_file = pm_tiny_log_file;
    pm_tiny_server.pm_tiny_app_log_dir = pm_tiny_app_log_dir;
    pm_tiny_server.pm_tiny_app_environ_dir = pm_tiny_app_environ_dir;
    pm_tiny_server.pm_tiny_sock_file = pm_tiny_cfg->pm_tiny_sock_file;
    pm_tiny_server.uds_abstract_namespace = pm_tiny_cfg->uds_abstract_namespace;
    try {
        pm_tiny_server.lmkdFd = pm_tiny::connect_lmkd();
    } catch (const std::exception &ex) {
        PM_TINY_LOG_E("connect lmkd error:%s", ex.what());
    }
    start(pm_tiny_server);
    delete_lock_pid_file(lock_fp, pm_tiny_lock_file.c_str());
    return 0;
}