#include "pm_tiny_server.h"
#include "log.h"
#include "globals.h"
#include "prog_cfg.h"
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <sys/prctl.h>
#include "android_lmkd.h"
#include "child_launch_context.h"

namespace pm_tiny {

bool pm_tiny_server_t::init_process_tree(const std::string &mode, const std::string &root) {
    if (!process_tree) process_tree = std::make_shared<process_tree_controller>();
    process_tree_mode requested;
    std::string reason;
    if (!parse_process_tree_mode(mode, requested)) {
        PM_TINY_LOG_E("invalid pm_tiny_process_tree_mode: %s", mode.c_str());
        return false;
    }
    std::string instance = pm_tiny_lock_file.empty() ? pm_tiny_home_dir : pm_tiny_lock_file;
    if (!process_tree_controller::enable_subreaper(reason)) {
        PM_TINY_LOG_E("child-subreaper unavailable, process tree cleanup is degraded: %s", reason.c_str());
    }
    if (!process_tree->initialize(requested, root, instance, reason)) {
        PM_TINY_LOG_E("process tree controller initialization failed: %s", reason.c_str());
        return false;
    }
    if (requested == process_tree_mode::auto_detect &&
        process_tree->effective_mode() == process_tree_mode::process_group) {
        PM_TINY_LOG_E("process tree degraded to process_group: %s", reason.c_str());
    }
    PM_TINY_LOG_I("process tree mode requested=%s effective=%s root=%s",
                  process_tree_mode_name(requested),
                  process_tree_mode_name(process_tree->effective_mode()),
                  process_tree->root().empty() ? "<process-group>" : process_tree->root().c_str());
    return true;
}

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
        std::string error_message;
        if (!rebuild_dependency_graph(pm_tiny_progs, error_message)) {
            PM_TINY_LOG_E("%s", error_message.c_str());
            return -1;
        }
        return 0;
    }

    std::unique_ptr<reload_config_t>
    pm_tiny_server_t::parse_cfg2() {
        proglist_t pl;
        parse_cfg(pl);
        dependency_graph graph;
        std::string error_message;
        const auto progs = std::vector<prog_ptr_t>(pl.begin(), pl.end());
        const bool valid = build_prog_dependency_graph(progs, graph, error_message);
        auto rconfig=std::make_unique<reload_config_t>(std::move(pl), std::move(graph), valid,
                                                       std::move(error_message));
        return rconfig;
    }

    bool pm_tiny_server_t::is_prog_depends_valid(prog_ptr_t prog) {
        const auto progs = std::vector<prog_ptr_t>(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end());
        std::vector<prog_ptr_t> candidate = progs;
        candidate.push_back(prog);
        dependency_graph graph;
        std::string error_message;
        const bool valid = build_prog_dependency_graph(candidate, graph, error_message);
        if (!valid) PM_TINY_LOG_E("%s", error_message.c_str());
        return valid;
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
                                         envs, kill_timeout_s, run_as, prog_cfg.pty);
            if (prog_info) {
                prog_info->depends_on = prog_cfg.depends_on;
                prog_info->start_timeout = prog_cfg.start_timeout;
                prog_info->failure_action = prog_cfg.failure_action;
                prog_info->daemon = prog_cfg.daemon;
                prog_info->oom_score_adj = prog_cfg.oom_score_adj;
                prog_info->heartbeat_timeout = prog_cfg.heartbeat_timeout;
                prog_info->env_vars = prog_cfg.env_vars;
                prog_info->restart_config.delay_ms = prog_cfg.restart_delay_ms;
                prog_info->restart_config.max_delay_ms = prog_cfg.restart_max_delay_ms;
                prog_info->restart_config.window_ms = prog_cfg.restart_window_ms;
                prog_info->restart_config.max_attempts = prog_cfg.restart_max_attempts;
                prog_info->restart_config.reset_after_ms = prog_cfg.restart_reset_after_ms;
                progs.push_back(prog_info.release());
            }
        }
        return 0;
    }

    std::unique_ptr<prog_info_t> pm_tiny_server_t::create_prog(const std::string &app_name,
                                             const std::string &cwd,
                                             const std::string &command,
                                             const std::vector<std::string> &envs,
                                             int kill_timeout_sec, const std::string &run_as,
                                             bool use_pty) const {
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        auto prog_info = std::make_unique<pm_tiny::prog_info_t>();
        prog_info->tree_controller = this->process_tree;
        prog_info->rpipefd[0] = prog_info->rpipefd[1] = -1;
        prog_info->logfile_fd[0] = prog_info->logfile_fd[1] = -1;
        prog_info->use_pty = use_pty;
        if (!use_pty) {
            prog_info->logfile[0] = app_log_dir;
            prog_info->logfile[0] += ("/" + app_name + "_stdout.log");
            prog_info->logfile[1] = app_log_dir;
            prog_info->logfile[1] += ("/" + app_name + "_stderr.log");
        } else {
            prog_info->logfile[0] = app_log_dir;
            prog_info->logfile[0] += ("/" + app_name + ".log");
            prog_info->logfile[1] = "";
        }
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
        prog_info->instance.pid = -1;
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
        proglist_t candidate = pm_tiny_progs;
        candidate.push_back(prog);
        dependency_graph graph;
        std::string error_message;
        const auto progs = std::vector<prog_ptr_t>(candidate.begin(), candidate.end());
        if (!build_prog_dependency_graph(progs, graph, error_message)) {
            PM_TINY_LOG_E("%s", error_message.c_str());
            return -1;
        }
        pm_tiny_progs.push_back(prog);
        dependency_graph_ = std::move(graph);
        dependency_runtime_.reset(dependency_graph_);
        for (const auto existing : pm_tiny_progs) {
            if (existing != prog && existing->state == PM_TINY_PROG_STATE_RUNING)
                dependency_runtime_.mark_ready(existing->name);
        }
        const auto names = dependency_runtime_.request_closure(prog->name);
        auto start_progs = progs_from_names(names);
        spawn0(start_progs);
        return 0;
    }

    int pm_tiny_server_t::start_prog(const prog_ptr_t &prog) {
        if (prog->instance.pid == -1) {
            int ret = spawn_prog(*prog);
            if (ret != -1) {
                prog->init_prog_log();
            }
            return ret;
        }
        return 1;
    }

    int pm_tiny_server_t::request_start(const prog_ptr_t &prog) {
        for (const auto existing : pm_tiny_progs) {
            if (existing->state == PM_TINY_PROG_STATE_RUNING)
                dependency_runtime_.mark_ready(existing->name);
        }
        auto start_progs = progs_from_names(dependency_runtime_.request_closure(prog->name));
        auto failures = spawn0(start_progs);
        return failures.empty() ? 0 : -1;
    }

    void pm_tiny_server_t::mark_dependency_stopped(const prog_ptr_t &prog) {
        dependency_runtime_.mark_idle(prog->name);
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
                          prog_cfg.oom_score_adj = p->oom_score_adj;
                          prog_cfg.pty = p->use_pty;
                          prog_cfg.restart_delay_ms = p->restart_config.delay_ms;
                          prog_cfg.restart_max_delay_ms = p->restart_config.max_delay_ms;
                          prog_cfg.restart_window_ms = p->restart_config.window_ms;
                          prog_cfg.restart_max_attempts = p->restart_config.max_attempts;
                          prog_cfg.restart_reset_after_ms = p->restart_config.reset_after_ms;
                          prog_cfgs.push_back(prog_cfg);

                      });
        return save_prog_cfg(prog_cfgs, cfg_path, this->pm_tiny_app_environ_dir);
    }

    void pm_tiny_server_t::restart_startfailed() {
        for (auto &prog: pm_tiny_progs) {
            if (prog->instance.pid == -1
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
            if (prog->instance.pid == -1) {
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

    void pm_tiny_server_t::async_kill_prog(prog_ptr_t&prog_){
        auto old_state = prog_->state;
        prog_->async_kill_prog();
        if (old_state == PM_TINY_PROG_STATE_STARTING) {
            PM_TINY_LOG_D("current prog name:%s,trigger DAG next",prog_->name.c_str());
            prog_->enqueue_after_termination(
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
        std::stringstream ss;
        for (auto p: started_progs) {
            ss << p->name << ",";
        }
        auto info = ss.str();
        if (!info.empty()) {
            info.erase(info.end() - 1);
        }
        PM_TINY_LOG_D("trigger node:%s", info.c_str());
        std::vector<std::string> ready_names;
        for (auto p : started_progs) {
            const auto unlocked = dependency_runtime_.mark_ready(p->name);
            ready_names.insert(ready_names.end(), unlocked.begin(), unlocked.end());
        }
        auto start_progs = progs_from_names(ready_names);
        if (start_progs.empty()) {
            PM_TINY_LOG_D("DAG next start empty");
            return;
        }
        this->spawn0(start_progs);
    }

    void pm_tiny_server_t::spawn() {
        for (auto &p: pm_tiny_progs) {
            p->state = PM_TINY_PROG_STATE_WAITING_START;
        }
        dependency_runtime_.reset(dependency_graph_);
        proglist_t start_progs = progs_from_names(dependency_runtime_.request_all());
        spawn0(start_progs);
    }

    void pm_tiny_server_t::close_fds() {
        for (auto &prog_info: pm_tiny_progs) {
            prog_info->close_fds(lmkdFd);
        }
    }

    int pm_tiny_server_t::real_spawn_prog(pm_tiny::prog_info_t &prog) {
        int tmp_errno;
        pm_tiny::child_launch_context launch;
        if (launch.prepare(prog.use_pty) == -1) {
            tmp_errno = errno;
            logger->syscall_errorlog(prog.use_pty ? "create pty" : "pipe");
            errno = tmp_errno;
            return -1;
        }
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
        pid_t pid = fork();
        if (pid < 0) {
            tmp_errno = errno;
            sigprocmask(SIG_SETMASK, &omask, nullptr);
            logger->syscall_errorlog("fork");
            launch.close_all();
            errno = tmp_errno;
            return -1;
        }
        if (pid > 0) {
            launch.close_parent_ends();
            sigprocmask(SIG_SETMASK, &omask, nullptr);
            prog.instance.begin(pid);
            std::string tree_reason;
            if (!this->process_tree->attach(pid, prog.instance.tree, tree_reason)) {
                PM_TINY_LOG_E("attach `%s` to process tree failed: %s", prog.name.c_str(), tree_reason.c_str());
                launch.close_all();
                kill(pid, SIGKILL);
                pm_tiny::safe_waitpid(pid, nullptr, 0);
                prog.instance.reset_failed_start();
                errno = EPERM;
                return -1;
            }
            if (prog.instance.tree.mode == process_tree_mode::process_group &&
                this->process_tree->effective_mode() == process_tree_mode::cgroup) {
                PM_TINY_LOG_I("`%s` process tree degraded to process_group: %s", prog.name.c_str(), tree_reason.c_str());
            }
            ssize_t gate_write = pm_tiny::safe_write(launch.gate[1], "1", 1);
            close(launch.gate[1]); launch.gate[1] = -1;
            if (gate_write != 1) {
                kill(pid, SIGKILL);
                pm_tiny::safe_waitpid(pid, nullptr, 0);
                close(launch.exec_status[0]); launch.exec_status[0] = -1;
                this->process_tree->cleanup(prog.instance.tree);
                prog.instance.reset_failed_start();
                return -1;
            }
            int child_errno = 0;
            ssize_t exec_status = pm_tiny::safe_read(launch.exec_status[0], &child_errno, sizeof(child_errno));
            close(launch.exec_status[0]); launch.exec_status[0] = -1;
            if (exec_status > 0) {
                pm_tiny::safe_waitpid(pid, nullptr, 0);
                this->process_tree->cleanup(prog.instance.tree);
                prog.instance.reset_failed_start();
                errno = child_errno;
                PM_TINY_LOG_E_SYS("`%s` startup fail", prog.name.c_str());
                return -1;
            }
            if (!prog.use_pty) {
                int stderr_fd = -1;
                launch.release_parent_streams(prog.rpipefd[0], stderr_fd);
                prog.rpipefd[1] = stderr_fd;
                int rc = pm_tiny::set_nonblock(prog.rpipefd[0]);
                if (rc == -1) {
                    logger->syscall_errorlog("fcntl");
                }
            } else {
                int unused_stderr = -1;
                launch.release_parent_streams(prog.rpipefd[0], unused_stderr);
            }
            prog.last_startup_ms = pm_tiny::time::gettime_monotonic_ms();
            if (lmkdFd) {
                lmk_procprio(this->lmkdFd.fd_, pid, get_uid_by_pid(pid), prog.oom_score_adj);
            }
            PM_TINY_LOG_I("startup `%s` pid:%d\n", prog.name.c_str(), pid);
        } else {
            launch.close_child_ends();
            const pid_t expected_parent = getppid();
            auto child_fail = [&](int error) {
                (void)pm_tiny::safe_write(launch.exec_status[1], &error, sizeof(error));
                _exit(112);
            };
            char gate = 0;
            while (read(launch.gate[0], &gate, 1) < 0 && errno == EINTR) {}
            close(launch.gate[0]); launch.gate[0] = -1;
            /* Reset signal handlers that were set by the parent process */
            reset_sighandlers_and_unblock_sigs();
            /* Create a new session and make ourselves the process group leader */
            if (setsid() == -1) child_fail(errno);
            if (!prog.use_pty) {
                int null_fd = open("/dev/null", O_RDWR);
                dup2(null_fd, STDIN_FILENO);
                dup2(launch.stdout_pipe[1], STDOUT_FILENO);
                dup2(launch.stderr_pipe[1], STDERR_FILENO);
                close(null_fd);
                close(launch.stdout_pipe[1]); launch.stdout_pipe[1] = -1;
                close(launch.stderr_pipe[1]); launch.stderr_pipe[1] = -1;
            } else {
                int pty_fd = open(launch.pty.slave_name, O_RDWR | O_CLOEXEC);
                if (pty_fd >= 0) {
                    dup2(pty_fd, STDIN_FILENO);
                    dup2(pty_fd, STDOUT_FILENO);
                    dup2(pty_fd, STDERR_FILENO);
                    close(pty_fd);
                    set_sane_term();
                } else {
                    child_fail(errno);
                }
            }
            int rc;
//            rc = pm_tiny::set_sigaction(SIGPIPE, SIG_DFL);
//            if (rc == -1) {
//                logger->syscall_errorlog("sigaction SIGPIPE");
//            }
            if (!prog.run_as.empty()) {
                passwd_t passwd;
                rc = get_uid_from_username(prog.run_as.c_str(), passwd);
                if (rc == -1) {
                    child_fail(errno);
                }
                rc = setreuid(passwd.pw_uid, passwd.pw_uid);
                if (rc == -1) {
                    child_fail(errno);
                }
            }
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) child_fail(errno);
            if (getppid() != expected_parent) child_fail(EPIPE);
            if (!prog.work_dir.empty()) {
                rc = chdir(prog.work_dir.c_str());
                if (rc == -1) {
                    child_fail(errno);
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
            child_fail(errno);
        }

        prog.state = PM_TINY_PROG_STATE_STARTING;
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
                                     return p->instance.pid == pid;
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
            dependency_runtime_.mark_idle(prog->name);
        }
    }

    prog_ptr_t pm_tiny_server_t::find_prog(const std::string &name) const {
        const auto found = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(), [&](prog_ptr_t prog) {
            return prog->name == name;
        });
        return found == pm_tiny_progs.end() ? nullptr : *found;
    }

    proglist_t pm_tiny_server_t::progs_from_names(const std::vector<std::string> &names) const {
        proglist_t result;
        for (const auto &name : names) {
            auto prog = find_prog(name);
            if (prog != nullptr) result.push_back(prog);
        }
        return result;
    }

    bool pm_tiny_server_t::rebuild_dependency_graph(const proglist_t &progs, std::string &error_message) {
        dependency_graph graph;
        const auto values = std::vector<prog_ptr_t>(progs.begin(), progs.end());
        if (!build_prog_dependency_graph(values, graph, error_message)) return false;
        dependency_graph_ = std::move(graph);
        dependency_runtime_.reset(dependency_graph_);
        for (const auto prog : pm_tiny_progs) {
            if (prog->state == PM_TINY_PROG_STATE_RUNING) dependency_runtime_.mark_ready(prog->name);
        }
        return true;
    }

    std::vector<std::string> pm_tiny_server_t::dependency_dependents(const std::string &name) const {
        std::vector<std::string> result;
        const auto id = dependency_graph_.find(name);
        if (id == dependency_graph::npos) return result;
        for (const auto dependent : dependency_graph_.dependents(id))
            result.push_back(dependency_graph_.name(dependent));
        return result;
    }

    void pm_tiny_server_t::remove_prog(prog_ptr_t &prog) {
        pm_tiny_progs.remove(prog);
        pm_tiny::delete_prog(prog);
        prog = nullptr;
        std::string error_message;
        if (!rebuild_dependency_graph(pm_tiny_progs, error_message))
            PM_TINY_LOG_E("failed to rebuild dependency graph after delete: %s", error_message.c_str());
    }

    void pm_tiny_server_t::flag_startup_fail(prog_ptr_t &prog) {
        const auto failure = dependency_runtime_.mark_failed(prog->name);
        prog->state = PM_TINY_PROG_STATE_STARTUP_FAIL;
        for (const auto &name : failure.blocked) {
            auto blocked = find_prog(name);
            if (blocked != nullptr) blocked->state = PM_TINY_PROG_STATE_BLOCKED;
        }
    }

    void pm_tiny_server_t::show_prog_depends_info() const {
        if (this->pm_tiny_progs.empty()) {
            PM_TINY_LOG_D("progs empty");
        } else {
            std::stringstream ss;
            for (const auto id : dependency_graph_.topological_order()) {
                ss << dependency_graph_.name(id) << " : ";
                const auto &dependencies = dependency_graph_.dependencies(id);
                for (std::size_t i = 0; i < dependencies.size(); ++i) {
                    if (i != 0) ss << ",";
                    ss << dependency_graph_.name(dependencies[i]);
                }
                ss << std::endl;
            }
            PM_TINY_LOG_I("depends:\n%s", ss.str().c_str());
        }
    }

    void pm_tiny_server_t::request_quit() {
        if (is_exiting()) {
            return;
        }
        this->server_exit = 1;
        for (auto prog: this->pm_tiny_progs) {
            prog->reset_restart_policy();
            if (prog->instance.pid != -1) {
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
        this->dependency_graph_ = std::move(this->reload_config->graph_);
        this->dependency_runtime_.reset(this->dependency_graph_);
        this->reload_config.reset();
    }

    bool pm_tiny_server_t::is_reloading() const {
        return server_exit != 0 && reload_config != nullptr;
    }

    bool pm_tiny_server_t::is_exiting() const {
        return this->server_exit != 0;
    }

    static bool xx_kill_1(prog_ptr_t &p, int signo) {
        bool find = false;
        if (p->instance.pid != -1 || !p->is_tree_empty()) {
            find = true;
//        logger->debug("kill %s(%d)", p->name.c_str(), p->pid);
            p->signal_tree(signo);
        }
        return find;
    }

    bool xx_wait_1(prog_ptr_t &p, int options) {
        bool find = false;
        if (p->instance.pid == -1) return !p->is_tree_empty();
        if (p->instance.pid != -1) {
            int wstatus;
            int rc = pm_tiny::safe_waitpid(p->instance.pid, &wstatus, options);
            if (rc == p->instance.pid) {
//          logger->debug("waitpid %s(%d)", p->name.c_str(), p->pid);
                p->last_wstatus = wstatus;
                p->instance.pid = -1;
                p->state = PM_TINY_PROG_STATE_STOPED;
            } else {
                find = true;
            }
        }
        return find;
    };
    void pm_tiny_server_t::kill_all_prog() {
        proglist_t &progs = this->pm_tiny_progs;
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
            auto max_iter = std::max_element(progs.begin(), progs.end(),
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
                      [&](prog_ptr_t &prog) {
                          prog->close_fds(this->lmkdFd);
                          prog->tree_controller->cleanup(prog->instance.tree);
                          prog->set_state(PM_TINY_PROG_STATE_STOPED);
                      });
    }
}
