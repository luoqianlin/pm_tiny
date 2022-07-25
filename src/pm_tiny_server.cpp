//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//
#include "pm_tiny_server.h"
#include "log.h"
#include "globals.h"
#include <termios.h>
#include <unistd.h>

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
        return parse_cfg(this->pm_tiny_progs);
    }

    void pm_tiny_server_t::parse_app_environ(const std::string &name,
                           std::vector <std::string> &envs) const {
        std::fstream efs(this->pm_tiny_app_environ_dir + "/" + name);
        if (!efs) {
            logger->debug("%s environ not exists", name.c_str());
            for (char **env = environ; *env != nullptr; env++) {
                envs.emplace_back(*env);
            }
            return;
        }

        for (std::string line; std::getline(efs, line);) {
            mgr::utils::trim(line);
            if (line.empty())continue;
            envs.emplace_back(line);
        }
    }

    int pm_tiny_server_t::parse_cfg(proglist_t &progs) const {
        const std::string &cfg_path = this->pm_tiny_cfg_file;
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        std::fstream cfg_file(cfg_path);
        if (!cfg_file) {
            logger->debug("not found cfg\n");
            return 0;
        }
        for (std::string line; std::getline(cfg_file, line);) {
            mgr::utils::trim(line);
            if (!line.empty() && line[0] != '#') {
                auto elements = mgr::utils::split(line, {':'});
                if (elements.size() < 3) {
                    continue;
                }
                for (auto &v: elements) {
                    mgr::utils::trim(v);
                }
                auto &app_name = elements[0];
                const auto iter = std::find_if(progs.begin(), progs.end(),
                                               [&app_name](const prog_ptr_t &prog) {
                                                   return prog->name == app_name;
                                               });
                if (iter != progs.end()) {
                    logger->info("name %s already exists ignore", app_name.c_str());
                    continue;
                }
                std::vector <std::string> envs;
                parse_app_environ(app_name, envs);
                int kill_timeout_s=3;
                std::string run_as;
                if (elements.size() > 3) {
                    try {
                        kill_timeout_s = std::stoi(elements[3]);
                    } catch (const std::exception &ex) {
                        //ignore
                    }
                    if (elements.size() > 4) {
                        run_as = elements[4];
                    }
                }
                auto prog_info = create_prog(app_name, elements[1], elements[2],
                                             envs,kill_timeout_s,run_as);
                if (prog_info) {
                    progs.push_back(prog_info);
                }
            }
        }
        return 0;
    }

    prog_ptr_t pm_tiny_server_t::create_prog(const std::string &app_name,
                           const std::string &cwd,
                           const std::string &command,
                           const std::vector <std::string> &envs,
                           int kill_timeout_sec,const std::string&run_as) const {
        const std::string &cfg_path = this->pm_tiny_cfg_file;
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        auto prog_info = std::make_shared<pm_tiny::prog_info_t>();
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
        if (kill_timeout_sec < 1 || kill_timeout_sec > 15) {
            kill_timeout_sec = 3;
        }
        prog_info->kill_timeout_sec = kill_timeout_sec;
        prog_info->run_as=run_as;
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
        const std::string &cfg_path = this->pm_tiny_cfg_file;
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        std::stringstream ss;
        std::vector <std::tuple<std::string, std::string>> f_envs;
        std::for_each(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end(),
                      [&ss, &f_envs](const prog_ptr_t &p) {
                          std::string command = std::accumulate(p->args.begin(), p->args.end(),
                                                                std::string(""),
                                                                [](const std::string &s1, const std::string &s2) {
                                                                    return s1 + (s2 + " ");
                                                                });
                          mgr::utils::trim(command);
                          ss << p->name << ":" << p->work_dir << ":" << command
                             << ":" << p->kill_timeout_sec << ":" << p->run_as << "\n";
                          std::stringstream env_ss;
                          for (auto &env: p->envs) {
                              env_ss << env << "\n";
                          }
                          f_envs.emplace_back(std::make_tuple(p->name, env_ss.str()));
                      });
        std::fstream cfg_file(cfg_path, std::ios::out | std::ios::trunc);
        if (!cfg_file) {
            logger->debug("not found cfg\n");
            return -1;
        }
        cfg_file << ss.str();
        for (auto &env: f_envs) {
            std::string f_name = std::get<0>(env);
            std::string content = std::get<1>(env);
            std::fstream env_fs(this->pm_tiny_app_environ_dir + "/" + f_name, std::ios::out | std::ios::trunc);
            if (!env_fs) {
                logger->debug("%s write fail", f_name.c_str());
            }
            env_fs << content;
        }
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

    void pm_tiny_server_t::spawn() {
        for (auto &prog: pm_tiny_progs) {
            if (prog->pid == -1) {
                auto retv = spawn_prog(*prog);
                if (retv == -1) {
                    continue;
                }
                prog->init_prog_log();
            }
        }
    }
    void  pm_tiny_server_t::close_fds() {
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
        mgr::utils::signal::sigprocmask_allsigs(SIG_BLOCK,&omask);
        pid_t pid = vfork();
        if (pid < 0) {
            tmp_errno = errno;
            sigprocmask(SIG_SETMASK,&omask, nullptr);
            logger->syscall_errorlog("vfork");
            errno = tmp_errno;
            return -1;
        }
        if (pid > 0) {
            sigprocmask(SIG_SETMASK,&omask, nullptr);
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
            logger->info("startup %s pid:%d\n", prog.name.c_str(), pid);
        } else {
            /* Reset signal handlers that were set by the parent process */
            reset_sighandlers_and_unblock_sigs();
            /* Create a new session and make ourself the process group leader */
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
                dup2(pty_fd, 0);
                dup2(pty_fd, 1);
                dup2(pty_fd, 2);
                close(pty_fd);
                set_sane_term();
            } else {
                failed = errno;
                _exit(112);
            }
#endif

            for (int i = getdtablesize() - 1; i > 2; --i) {
                close(i);
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
            for (int i = 0; i < prog.args.size() && i < (sizeof(args) / sizeof(args[0])); i++) {
                args[i] = (char *) prog.args[i].c_str();
            }
            char *envp[4096];
            memset(&envp, 0, sizeof(envp));
            for (int i = 0; i < prog.envs.size()
                            && (i < ((sizeof(envp) / sizeof(envp[0])) - 1)); i++) {
                envp[i] = (char *) prog.envs[i].data();
            }
            execvpe(args[0], args, envp);
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
            logger->syscall_errorlog("%s startup fail", prog.name.c_str());
            errno = failed;
            return -1;
        } else {
            prog.state = PM_TINY_PROG_STATE_RUNING;
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
                logger->syscall_errorlog("cannot open %s dir",procdir_path);
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
                    auto is_equal = [](const std::vector <std::string> &v1,
                                       const std::vector <std::string> &v2) {
                        if (v1.size() != v2.size())return false;
                        for (int i = 0; i < v1.size(); i++) {
                            if (v1[i] != v2[i]) {
                                return false;
                            }
                        }
                        return true;
                    };
                    if (is_equal(procinfo.cmdline, prog.args)) {
                        auto cmd=mgr::utils::join(procinfo.cmdline);
                        PM_TINY_LOG_I("found detach pid:%d exe:%s cmdline:%s comm:%s",
                                     pid, procinfo.exe_path.c_str(),
                                     cmd.c_str(), procinfo.comm.c_str());
                        pm_tiny::safe_kill_process(pid,prog.kill_timeout_sec);
                    }
                } else {
//                logger->syscall_errorlog("get_exe_path");
                }
            }
            closedir(procdir);
        } while (false);
        return real_spawn_prog(prog);
    }
}