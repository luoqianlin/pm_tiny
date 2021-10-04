//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//
#include "pm_tiny_server.h"
#include "log.h"
#include "globals.h"
#include "prog_cfg.h"
#include <termios.h>
#include <unistd.h>
#include <algorithm>

namespace pm_tiny {

    static void reset_sighandlers_and_unblock_sigs() {
        mgr::utils::signal::bb_signals(0
                                       + (1 << SIGCHLD)
                                       + (1 << SIGALRM)
                                       + (1 << SIGTERM)
                                       + (1 << SIGQUIT)
                                       + (1 << SIGINT)
                                       + (1 << SIGHUP)
                                       + (1 << SIGTSTP)
                                       + (1 << SIGSTOP)
                                       + (1 << SIGPIPE), SIG_DFL);
//        /* Setup default signals for the new process */
//        for (int i = 1; i <= NSIG; i++)
//            signal(i, SIG_DFL);
        mgr::utils::signal::sigprocmask_allsigs(SIG_UNBLOCK, nullptr);
    }


    int pm_tiny_server_t::parse_cfg() {
        parse_cfg(this->pm_tiny_progs);
        auto progs = std::vector<prog_ptr_t>(pm_tiny_progs.begin(), pm_tiny_progs.end());
        this->progDAG = check_prog_info(progs);
        return this->progDAG != nullptr ? 0 : -1;
    }

    std::unique_ptr<reload_config_t>
    pm_tiny_server_t::parse_cfg2() {
        proglist_t pl;
        parse_cfg(pl);
        auto progs = std::vector<prog_ptr_t>(pl.begin(), pl.end());
        auto dag = check_prog_info(progs);
        auto rconfig=std::make_unique<reload_config_t>(std::move(pl),std::move(dag));
        return rconfig;
    }

    bool pm_tiny_server_t::is_prog_depends_valid(prog_ptr_t prog) {
        auto progs = std::vector<prog_ptr_t>(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end());
        progs.push_back(prog);
        auto dag = check_prog_info(progs);
        return dag != nullptr;
    }

    void pm_tiny_server_t::parse_app_environ(const std::string &name,
                                             std::vector<std::string> &envs) const {
        auto app_envs = load_app_environ(name, this->pm_tiny_app_environ_dir);
        for (auto &env: app_envs) {
            envs.push_back(env);
        }
    }

    int pm_tiny_server_t::parse_cfg(proglist_t &progs) const {
        const std::string &cfg_path = this->pm_tiny_prog_cfg_file;
        auto prog_cfgs = load_prog_cfg(cfg_path,
                                       this->pm_tiny_app_environ_dir);

        for (const auto &prog_cfg: prog_cfgs) {
            auto &app_name = prog_cfg.name;
            const auto iter = std::find_if(progs.begin(), progs.end(),
                                           [&app_name](const prog_ptr_t &prog) {
                                               return prog->name == app_name;
                                           });
            if (iter != progs.end()) {
                PM_TINY_LOG_I("name %s already exists ignore", app_name.c_str());
                continue;
            }
            std::vector<std::string> envs = prog_cfg.envs;
            int kill_timeout_s = prog_cfg.kill_timeout_s;
            std::string run_as = prog_cfg.run_as;
            auto prog_info = create_prog(app_name, prog_cfg.cwd, prog_cfg.command,
                                         envs, kill_timeout_s, run_as);
            if (prog_info) {
                prog_info->depends_on = prog_cfg.depends_on;
                prog_info->start_timeout = prog_cfg.start_timeout;
                prog_info->failure_action = prog_cfg.failure_action;
                prog_info->daemon = prog_cfg.daemon;
                prog_info->heartbeat_timeout = prog_cfg.heartbeat_timeout;
                prog_info->env_vars = prog_cfg.env_vars;
                progs.push_back(prog_info.release());
            }
        }
        return 0;
    }

    std::unique_ptr<prog_info_t> pm_tiny_server_t::create_prog(const std::string &app_name,
                                             const std::string &cwd,
                                             const std::string &command,
                                             const std::vector<std::string> &envs,
                                             int kill_timeout_sec, const std::string &run_as) const {
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        auto prog_info = std::make_unique<pm_tiny::prog_info_t>();
        prog_info->rpipefd[0] = prog_info->rpipefd[1] = -1;
        prog_info->logfile_fd[0] = prog_info->logfile_fd[1] = -1;
#if !PM_TINY_PTY_ENABLE
        prog_info->logfile[0] = app_log_dir;
        prog_info->logfile[0] += ("/" + app_name + "_stdout.log");
        prog_info->logfile[1] = app_log_dir;
        prog_info->logfile[1] += ("/" + app_name + "_stderr.log");
#else
        prog_info->logfile[0] = app_log_dir;
        prog_info->logfile[0] += ("/" + app_name + ".log");
        prog_info->logfile[1] = "";
#endif
        prog_info->name = app_name;
        prog_info->work_dir = cwd;
        prog_info->dead_count = 0;
        prog_info->last_dead_time_ms = 0;
        prog_info->args = mgr::utils::split(command, {' ', '\t'});
        prog_info->args.erase(
                std::remove_if(prog_info->args.begin(), prog_info->args.end(),
                               [](const std::string &arg) {
                                   return mgr::utils::trim_copy(arg).empty();
                               }), prog_info->args.end());
        prog_info->pid = -1;
        prog_info->envs = envs;
        if (prog_info->work_dir.empty()) {
            logger->info("%s work dir is empty ignore", app_name.c_str());
            return nullptr;
        }
        if (prog_info->args.empty()) {
            logger->info("%s args is empty ignore", app_name.c_str());
            return nullptr;
        }
        if (kill_timeout_sec < 1) {
            kill_timeout_sec = 3;
        }
        prog_info->kill_timeout_sec = kill_timeout_sec;
        prog_info->run_as = run_as;
        return prog_info;
    }

    int pm_tiny_server_t::start_and_add_prog(const prog_ptr_t &prog) {
        int ret = start_prog(prog);
        if (ret != -1) {
            this->pm_tiny_progs.push_back(prog);
        }
        return ret;
    }

    int pm_tiny_server_t::start_prog(const prog_ptr_t &prog) {
        if (prog->pid == -1) {
            if (prog->state == PM_TINY_PROG_STATE_WAITING_START) {
                this->remove_from_DAG(prog);
            }
            int ret = spawn_prog(*prog);
            if (ret != -1) {
                prog->init_prog_log();
            }
            return ret;
        }
        return 1;
    }

/* Set terminal settings to reasonable defaults.
 * NB: careful, we can be called after vfork! */
    static void set_sane_term(void) {
        struct termios tty;

        tcgetattr(STDIN_FILENO, &tty);

        /* set control chars */
        tty.c_cc[VINTR] = 3;    /* C-c */
        tty.c_cc[VQUIT] = 28;    /* C-\ */
        tty.c_cc[VERASE] = 127;    /* C-? */
        tty.c_cc[VKILL] = 21;    /* C-u */
        tty.c_cc[VEOF] = 4;    /* C-d */
        tty.c_cc[VSTART] = 17;    /* C-q */
        tty.c_cc[VSTOP] = 19;    /* C-s */
        tty.c_cc[VSUSP] = 26;    /* C-z */

#ifdef __linux__
        /* use line discipline 0 */
        tty.c_line = 0;
#endif

        /* Make it be sane */
#ifndef CRTSCTS
# define CRTSCTS 0
#endif
        /* added CRTSCTS to fix Debian bug 528560 */
        tty.c_cflag &= CBAUD | CBAUDEX | CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS;
        tty.c_cflag |= CREAD | HUPCL | CLOCAL;

        /* input modes */
        tty.c_iflag = ICRNL | IXON | IXOFF;

        /* output modes */
//	tty.c_oflag = OPOST | ONLCR;
        tty.c_oflag = OPOST | OCRNL;

        /* local modes */
        tty.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;

        tcsetattr_stdin_TCSANOW(&tty);
    }

    int pm_tiny_server_t::save_proc_to_cfg() {
        //name:cwd:command
        const std::string &cfg_path = this->pm_tiny_prog_cfg_file;
        std::vector<prog_cfg_t> prog_cfgs;

        std::for_each(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end(),
                      [&prog_cfgs](const prog_ptr_t &p) {
                          std::string command = std::accumulate(p->args.begin(), p->args.end(),
                                                                std::string(""),
                                                                [](const std::string &s1, const std::string &s2) {
                                                                    return s1 + (s2 + " ");
                                                                });
                          mgr::utils::trim(command);
                          prog_cfg_t prog_cfg;
                          prog_cfg.command = command;
                          prog_cfg.cwd = p->work_dir;
                          prog_cfg.name = p->name;
                          prog_cfg.kill_timeout_s = p->kill_timeout_sec;
                          prog_cfg.run_as = p->run_as;
                          prog_cfg.envs = p->envs;
                          prog_cfg.depends_on = p->depends_on;
                          prog_cfg.daemon = p->daemon;
                          prog_cfg.start_timeout = p->start_timeout;
                          prog_cfg.failure_action = p->failure_action;
                          prog_cfg.heartbeat_timeout = p->heartbeat_timeout;
                          prog_cfg.env_vars = p->env_vars;
                          prog_cfgs.push_back(prog_cfg);

                      });
        save_prog_cfg(prog_cfgs, cfg_path, this->pm_tiny_app_environ_dir);
        return 0;
    }

    void pm_tiny_server_t::restart_startfailed() {
        for (auto &prog: pm_tiny_progs) {
            if (prog->pid == -1
                && prog->state == PM_TINY_PROG_STATE_STARTUP_FAIL) {
                auto retv = spawn_prog(*prog);
                if (retv == -1) {
                    continue;
                }
                prog->init_prog_log();
            }
        }
    }

    proglist_t pm_tiny_server_t::spawn0(proglist_t &start_progs) {
        proglist_t fail_progs;
        std::stringstream ss;
        ss<<"[";
        for (auto &prog: start_progs) {
            ss<<prog->name<<" ";
            if (prog->pid == -1) {
                auto retv = spawn_prog(*prog);
                if (retv == -1) {
                    fail_progs.push_back(prog);
                    flag_startup_fail(prog);
                    continue;
                }
                prog->init_prog_log();
            }
        }
        ss<<"]";
        PM_TINY_LOG_D("start:%s",ss.str().c_str());
        return fail_progs;
    }

    auto get_alarm_time = [](proglist_t &start_progs,
                             proglist_t &fail_progs) {
        for (const auto &p: fail_progs) {
            start_progs.remove(p);
        }
        auto alarm_time = get_min_start_timeout(start_progs);
        return alarm_time;
    };
    void pm_tiny_server_t::async_kill_prog(prog_ptr_t&prog_){
        auto old_state = prog_->state;
        prog_->async_kill_prog();
        if (old_state == PM_TINY_PROG_STATE_STARTING
            && !this->progDAG->is_traversal_complete()) {
            PM_TINY_LOG_D("current prog name:%s,trigger DAG next",prog_->name.c_str());
            prog_->kill_pendingtasks.emplace_back(
                    [prog_](pm_tiny_server_t &pm_tiny_server) {
                        if (pm_tiny_server.is_exiting()) {
                            return;
                        }
                        proglist_t pl;
                        pl.push_back(prog_);
                        pm_tiny_server.spawn1(pl);
                    });
        }
    }

    void pm_tiny_server_t::spawn1(proglist_t &started_progs) {
//        while (!started_progs.empty()) {
        if (this->progDAG->is_traversal_complete()) {
            PM_TINY_LOG_D("traversal complete");
            return;
        }
        std::stringstream ss;
        for (auto p: started_progs) {
            ss << p->name << ",";
        }
        auto info = ss.str();
        if (!info.empty()) {
            info.erase(info.end() - 1);
        }
        PM_TINY_LOG_D("trigger node:%s", info.c_str());
        auto start_progs = this->progDAG->next(started_progs);
        if (start_progs.empty()) {
            PM_TINY_LOG_D("DAG next start empty");
            return;
        }
        this->spawn0(start_progs);
//        }
    }

    void pm_tiny_server_t::spawn() {
        for (auto &p: pm_tiny_progs) {
            p->state = PM_TINY_PROG_STATE_WAITING_START;
        }
        proglist_t start_progs = this->progDAG->start();
        spawn0(start_progs);
    }

    void pm_tiny_server_t::close_fds() {
        for (auto &prog_info: pm_tiny_progs) {
            prog_info->close_fds();
        }
    }

    int pm_tiny_server_t::real_spawn_prog(pm_tiny::prog_info_t &prog) {
        volatile int failed = 0;
        int tmp_errno;
#if !PM_TINY_PTY_ENABLE
        int pipefd[2];
        int pipefd2[2];
        if (pipe(pipefd) == -1) {
            tmp_errno = errno;
            logger->syscall_errorlog("pipe");
            errno = tmp_errno;
            return -1;
        }
        if (pipe(pipefd2) == -1) {
            tmp_errno = errno;
            close(pipefd[0]);
            close(pipefd[1]);
            logger->syscall_errorlog("pipe");
            errno = tmp_errno;
            return -1;
        }
#else
        struct pm_tiny::pty_info pti;
        if (pm_tiny::create_pty(&pti) != 0) {
            tmp_errno = errno;
            logger->syscall_errorlog("create pty");
            errno = tmp_errno;
            return -1;
        }
#endif
        sigset_t omask;
        /* Careful: don't be affected by a signal in vforked child */
        mgr::utils::signal::sigprocmask_allsigs(SIG_BLOCK, &omask);
        std::vector<char *> envp;
        envp.reserve(prog.envs.size() + prog.env_vars.size() + 5);
        std::string app_id_env = PM_TINY_APP_NAME "=";
        app_id_env += prog.name;
        std::string home_dir_env = PM_TINY_HOME "=";
        home_dir_env += this->pm_tiny_home_dir;
        std::string socket_path = PM_TINY_SOCK_FILE "=";
        socket_path += this->pm_tiny_sock_file;
        std::string uds_an = PM_TINY_UDS_ABSTRACT_NAMESPACE "=";
        if (this->uds_abstract_namespace) {
            uds_an += "1";
        } else {
            uds_an += "0";
        }
        envp.push_back(const_cast<char *>(app_id_env.c_str()));
        envp.push_back(const_cast<char *>(home_dir_env.c_str()));
        envp.push_back(const_cast<char *>(socket_path.c_str()));
        envp.push_back(const_cast<char *>(uds_an.c_str()));
        pid_t pid = vfork();
        if (pid < 0) {
            tmp_errno = errno;
            sigprocmask(SIG_SETMASK, &omask, nullptr);
            logger->syscall_errorlog("vfork");
            errno = tmp_errno;
            return -1;
        }
        if (pid > 0) {
            sigprocmask(SIG_SETMASK, &omask, nullptr);
            prog.pid = pid;
            prog.backup_pid = pid;
#if !PM_TINY_PTY_ENABLE
            close(pipefd[1]);
            close(pipefd2[1]);
            prog.rpipefd[0] = pipefd[0];
            prog.rpipefd[1] = pipefd2[0];
            int rc = pm_tiny::set_nonblock(pipefd[0]);
            if (rc == -1) {
                logger->syscall_errorlog("fcntl");
            }
#else
            prog.rpipefd[0] = pti.master_fd;
#endif
            prog.last_startup_ms = pm_tiny::time::gettime_monotonic_ms();
            PM_TINY_LOG_I("startup `%s` pid:%d\n", prog.name.c_str(), pid);
        } else {
            /* Reset signal handlers that were set by the parent process */
            reset_sighandlers_and_unblock_sigs();
            /* Create a new session and make ourselves the process group leader */
            setsid();
#if !PM_TINY_PTY_ENABLE
            close(pipefd[0]);
            close(pipefd2[0]);
            int null_fd = open("/dev/null", O_RDWR);
            dup2(null_fd, STDIN_FILENO);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd2[1], STDERR_FILENO);
            close(null_fd);
            close(pipefd[1]);
            close(pipefd2[1]);
#else
            close(pti.master_fd);
            int pty_fd = open(pti.slave_name, O_RDWR | O_CLOEXEC);
            if (pty_fd >= 0) {
                dup2(pty_fd, STDIN_FILENO);
                dup2(pty_fd, STDOUT_FILENO);
                dup2(pty_fd, STDERR_FILENO);
                close(pty_fd);
                set_sane_term();
            } else {
                failed = errno;
                _exit(112);
            }
#endif
            int rc;
//            rc = pm_tiny::set_sigaction(SIGPIPE, SIG_DFL);
//            if (rc == -1) {
//                logger->syscall_errorlog("sigaction SIGPIPE");
//            }
            if (!prog.run_as.empty()) {
                passwd_t passwd;
                rc = get_uid_from_username(prog.run_as.c_str(), passwd);
                if (rc == -1) {
                    failed = errno;
                    _exit(112);
                }
                rc = setreuid(passwd.pw_uid, passwd.pw_uid);
                if (rc == -1) {
                    failed = errno;
                    _exit(112);
                }
            }
            if (!prog.work_dir.empty()) {
                rc = chdir(prog.work_dir.c_str());
                if (rc == -1) {
                    failed = errno;
                    _exit(112);
                }
            }
            char *args[80] = {nullptr};
            for (int i = 0; i < static_cast<int>(prog.args.size())
                            && i < static_cast<int>(sizeof(args) / sizeof(args[0])); i++) {
                args[i] = (char *) prog.args[i].c_str();
            }
            auto add2envp = [&](const std::string &env) {
                if (env.rfind("PM_TINY_", 0) == 0) {
                    return;
                }
                envp.push_back(const_cast<char *>(env.c_str()));
            };
            std::for_each(prog.env_vars.begin(), prog.env_vars.end(), add2envp);
            std::for_each(prog.envs.begin(), prog.envs.end(), add2envp);
            envp.push_back(nullptr);
            execvpe(args[0], args, envp.data());
            failed = errno;
            _exit(111);
        }

        if (failed) {
            prog.pid = -1;
            prog.backup_pid = -1;
            prog.close_pipefds();
            pm_tiny::safe_waitpid(pid, nullptr, 0); /* prevent zombie */
            errno = failed;
            prog.state = PM_TINY_PROG_STATE_STARTUP_FAIL;
            PM_TINY_LOG_E_SYS("`%s` startup fail", prog.name.c_str());
            errno = failed;
            return -1;
        } else {
            prog.state = PM_TINY_PROG_STATE_STARTING;
        }
        return 0;
    }

    static int isnumeric(char *str) {
        int i = 0;

        // Empty string is not numeric
        if (str[0] == 0)
            return 0;

        while (1) {
            if (str[i] == 0) // End of string
                return 1;

            if (isdigit(str[i]) == 0)
                return 0;

            i++;
        }
    }

    prog_ptr_t pm_tiny_server_t::find_prog(int pid) {
        auto iter = std::find_if(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end(),
                                 [&pid](const prog_ptr_t &p) {
                                     return p->pid == pid;
                                 });
        if (iter == this->pm_tiny_progs.end()) {
            return nullptr;
        }
        return *iter;
    }

    int pm_tiny_server_t::spawn_prog(pm_tiny::prog_info_t &prog) {
        do {
            DIR *procdir = opendir(procdir_path);
            if (procdir == nullptr) {
                logger->syscall_errorlog("cannot open %s dir", procdir_path);
                break;
            }
            while (true) {
                errno = 0;
                struct dirent *d = readdir(procdir);
                if (d == nullptr) {
                    if (errno != 0) {
                        logger->syscall_errorlog("readdir");
                    }
                    break;
                }
                // proc contains lots of directories not related to processes,
                // skip them
                if (!isnumeric(d->d_name))
                    continue;
                int pid = (int) strtol(d->d_name, nullptr, 10);
                if (pid == getpid() || this->find_prog(pid)) {
                    continue;
                }
                pm_tiny::utils::proc::procinfo_t procinfo;
                int rc = pm_tiny::utils::proc::get_proc_info(pid, procinfo);
                if (rc == 0) {
                    using namespace std::string_literals;
                    auto is_equal = [](const std::vector<std::string> &v1,
                                       const std::vector<std::string> &v2) {
                        if (v1.size() != v2.size())return false;
                        for (std::vector<std::string>::size_type i = 0; i < v1.size(); i++) {
                            if (v1[i] != v2[i]) {
                                return false;
                            }
                        }
                        return true;
                    };
                    if (is_equal(procinfo.cmdline, prog.args)) {
                        auto cmd = mgr::utils::join(procinfo.cmdline);
                        PM_TINY_LOG_I("found detach pid:%d exe:%s cmdline:%s comm:%s",
                                      pid, procinfo.exe_path.c_str(),
                                      cmd.c_str(), procinfo.comm.c_str());
                        pm_tiny::safe_kill_process(pid, prog.kill_timeout_sec);
                    }
                } else {
//                logger->syscall_errorlog("get_exe_path");
                }
            }
            closedir(procdir);
        } while (false);
        return real_spawn_prog(prog);
    }

    void pm_tiny_server_t::trigger_DAG_traversal_next_node(const prog_ptr_t &prog) {
        if (prog->state == PM_TINY_PROG_STATE_WAITING_START) {
            proglist_t pl;
            pl.push_back(prog);
            this->progDAG->remove(pl);
            this->spawn1(pl);
        }
    }

    void pm_tiny_server_t::remove_from_DAG(const prog_ptr_t &prog) {
        PM_TINY_LOG_D("%s",prog->name.c_str());
        proglist_t pl;
        pl.push_back(prog);
        this->progDAG->remove(pl);
    }

    void pm_tiny_server_t::remove_prog(prog_ptr_t &prog) {
        pm_tiny_progs.remove(prog);
        for (auto &p: pm_tiny_progs) {
            auto &deps = p->depends_on;
            auto iter = std::find(deps.begin(), deps.end(), prog->name);
            if (iter != deps.end()) {
                deps.erase(iter);
            }
        }
        pm_tiny::delete_prog(prog);
        prog = nullptr;
    }

    void pm_tiny_server_t::flag_startup_fail(prog_ptr_t &prog) const {
        auto &graph = progDAG->graph;
        auto vertex_i = graph->vertex_index([&](auto &wrapper) {
            return wrapper.prog_info == prog;
        });
        if (vertex_i != pm_tiny::prog_graph_t::npos) {
            PM_TINY_LOG_D("vertex_i:%d", vertex_i);
            auto vertices = graph->bfs(vertex_i);
            std::sort(vertices.begin(), vertices.end(), std::greater<>());
            std::stringstream ss;
            for (auto ver: vertices) {
                ss << ver << ",";
            }
            PM_TINY_LOG_D("delete:%s", ss.str().c_str());
            for (auto ver: vertices) {
                graph->vertex(ver).prog_info->state = PM_TINY_PROG_STATE_STARTUP_FAIL;
                graph->remove_vertex(ver);
            }
        }
    }

    void pm_tiny_server_t::show_prog_depends_info() const {
        if (this->pm_tiny_progs.empty()) {
            PM_TINY_LOG_D("progs empty");
        } else {
            if (this->progDAG) {
                this->progDAG->show_depends_info();
            }
        }
    }

    void pm_tiny_server_t::request_quit() {
        if (is_exiting()) {
            return;
        }
        this->server_exit = 1;
        for (auto prog: this->pm_tiny_progs) {
            if (prog->pid != -1) {
                prog->async_kill_prog();
                prog->execute_penddingtasks(*this);
            }
            if (prog->state == PM_TINY_PROG_STATE_WAITING_START) {
                prog->state = PM_TINY_PROG_STATE_NO_RUN;
            }
        }
    }

    void pm_tiny_server_t::swap_reload_config() {
        this->pm_tiny_progs = std::move(this->reload_config->pl_);
        this->progDAG = std::move(this->reload_config->dag_);
        this->reload_config.reset();
    }

    bool pm_tiny_server_t::is_reloading() const {
        return server_exit != 0 && reload_config != nullptr;
    }

    bool pm_tiny_server_t::is_exiting() const {
        return this->server_exit != 0;
    }
}