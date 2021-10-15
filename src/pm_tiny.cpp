#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>
#include <numeric>
#include <list>
#include <tuple>
#include <string.h>
#include <stdarg.h>
#include "logger.hpp"
#include <memory>
#include "../src/string_utils.h"
#include <math.h>
#include <climits>
#include "session.h"
#include "pm_sys.h"
#include "pm_tiny.h"
#include "pm_tiny_helper.h"
#include "frame_stream.hpp"


std::shared_ptr<pm_tiny::logger_t> logger;


namespace pm_tiny {
    logger_t logger_stdout(STDOUT_FILENO);
    logger_t logger_stderr(STDERR_FILENO);

    auto f_close = [](int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };
    auto f_log_open = [](std::string &path, int oflag = O_CREAT | O_RDWR | O_APPEND) {
        const char *file = path.c_str();
        int fd = open(file, oflag, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            logger->syscall_errorlog("open");
        }
        return fd;
    };
    auto get_file_size = [](int fd) {
        struct stat st;
        int rc = fstat(fd, &st);
        if (rc == -1) {
            logger->syscall_errorlog("fstat");
        }
        return st.st_size;
    };

    struct prog_info_t {
        pid_t pid = -1;
        pid_t backup_pid = -1;
        int rpipefd[2]{-1, -1};
        std::vector<std::string> args;
        int64_t last_startup_ms = 0;
        int64_t last_dead_time_ms = 0;
        int last_wstatus = 0;
        int pendding_signal = 0;
        int dead_count = 0;
        int dead_count_timer = 0;
        std::string name;
        std::string logfile[2];
        std::string work_dir;
        int logfile_fd[2]{-1, -1};
        int64_t logfile_size[2]{0, 0};//bytes
        int64_t logfile_maxsize = 4 * 1024 * 1024L;
        int logfile_count = 3;
        int64_t moniter_duration_threshold = 60 * 1000L;
        int64_t min_lifetime_threshold = 100L;
        int moniter_duration_max_dead_count = -1;
        int state = PM_TINY_PROG_STATE_NO_RUN;
        std::vector<std::string> envs;

        void close_pipefds() {
            std::for_each(std::begin(this->rpipefd),
                          std::end(this->rpipefd),
                          pm_tiny::f_close);
        }

        void close_logfds() {
            std::for_each(std::begin(this->logfile_fd),
                          std::end(this->logfile_fd),
                          pm_tiny::f_close);
        }

        void set_state(int s) {
            this->state = s;
        }

        /**
         * 监管的程序运行结束后会关闭pipefd,
         * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
         * */
        void close_fds() {
            for (int i = 0; i < 2; i++) {
                if (this->rpipefd[i] != -1
                    && this->logfile_fd[i] != -1) {
//                    logger->debug("start safe close fds");
                    read_pipe(i);
//                    logger->debug("end safe close fds");
                }
            }
            this->close_pipefds();
            this->close_logfds();
            this->pid = -1;
        }

        std::string get_dsc_name() const {
            return this->name + "(" + std::to_string(this->backup_pid) + ")";
        }

        void init_prog_log() {
            for (int i = 0; i < 2; i++) {
                this->logfile_fd[i] = f_log_open(this->logfile[i]);
                this->logfile_size[i] = get_file_size(this->logfile_fd[i]);
                logger->info("log file %s  %ld bytes\n",
                             this->logfile[i].c_str(), this->logfile_size[i]);
            }
        }

        void read_pipe(int i) {
            int nread;
            int rc;
            char buffer[4096];
            int &fd = this->rpipefd[i];
            ioctl(fd, FIONREAD, &nread);
            if (nread == 0) {
                close(fd);
                close(this->logfile_fd[i]);
                logger->debug("pid:%d pipe fd %d closed\n",
                              this->backup_pid, fd);
                fd = -1;
                this->logfile_fd[i] = -1;
            } else {
                int remaining_bytes = nread;
                do {
                    int max_nread = std::min(remaining_bytes, (int) sizeof(buffer));
                    rc = (int) pm_tiny::safe_read(fd, buffer, max_nread);
                    if (rc > 0) {
                        this->logfile_size[i] += rc;
                        if (this->logfile_size[i] > this->logfile_maxsize) {
//                            logger->info("exceeds the maximum file size of %ld bytes,truncate\n",
//                                         this->logfile_maxsize);
                            close(this->logfile_fd[i]);
                            pm_tiny::logger_t::logfile_cycle_write(this->logfile[i], this->logfile_count);
                            this->logfile_fd[i] = pm_tiny::f_log_open(this->logfile[i], O_CREAT | O_RDWR | O_TRUNC);
                            this->logfile_size[i] = rc;
                        }
                        pm_tiny::safe_write(this->logfile_fd[i], buffer, rc);
                        remaining_bytes -= rc;
                    } else if ((rc == -1 && errno != EINTR)) {
                        logger->syscall_errorlog("name:%s pid:%d read", this->name.c_str(), this->pid);
                        break;
                    }
                } while (remaining_bytes > 0);
            }
        }
    };

    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog) {
        os << "name:'" << prog.name + "'" << " pid:" << prog.pid << " ";
        os << "work dir:" << prog.work_dir << " ";
        os << "args: ";
        std::for_each(std::begin(prog.args), std::end(prog.args), [&os](const std::string &s) {
            os << s << " ";
        });
        return os;
    }
}
using prog_ptr_t = std::shared_ptr<pm_tiny::prog_info_t>;
using proglist_t = std::list<prog_ptr_t>;


int spawn_prog(pm_tiny::prog_info_t &prog);

void kill_prog(proglist_t &progs);


struct pm_tiny_server_t {
    std::string pm_tiny_home_dir;
    std::string pm_tiny_log_file;
    std::string pm_tiny_cfg_file;
    std::string pm_tiny_app_log_dir;
    std::string pm_tiny_app_environ_dir;
    proglist_t pm_tiny_progs;

    int parse_cfg() {
        return parse_cfg(this->pm_tiny_progs);
    }

    void parse_app_environ(const std::string &name,
                           std::vector<std::string> &envs) const {
        std::fstream efs(this->pm_tiny_app_environ_dir + "/" + name);
        if (!efs) {
            logger->debug("%s environ not exists", name.c_str());
            return;
        }

        for (std::string line; std::getline(efs, line);) {
            mgr::utils::trim(line);
            if (line.empty())continue;
            envs.emplace_back(line);
        }
    }

    int parse_cfg(proglist_t &progs) const {
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
                std::vector<std::string> envs;
                parse_app_environ(app_name, envs);
                auto prog_info = create_prog(app_name, elements[1], elements[2], envs);
                if (prog_info) {
                    progs.push_back(prog_info);
                }
            }
        }
        return 0;
    }

    prog_ptr_t create_prog(const std::string &app_name,
                           const std::string &cwd,
                           const std::string &command,
                           const std::vector<std::string> &envs) const {
        const std::string &cfg_path = this->pm_tiny_cfg_file;
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        auto prog_info = std::make_shared<pm_tiny::prog_info_t>();
        prog_info->rpipefd[0] = prog_info->rpipefd[1] = -1;
        prog_info->logfile_fd[0] = prog_info->logfile_fd[1] = -1;
        prog_info->logfile[0] = app_log_dir;
        prog_info->logfile[0] += ("/" + app_name + "_stdout.log");
        prog_info->logfile[1] = app_log_dir;
        prog_info->logfile[1] += ("/" + app_name + "_stderr.log");
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
        return prog_info;
    }

    int start_and_add_prog(const prog_ptr_t &prog) {
        int ret = start_prog(prog);
        if (ret != -1) {
            this->pm_tiny_progs.push_back(prog);
        }
        return ret;
    }

    static int start_prog(const prog_ptr_t &prog) {
        if (prog->pid == -1) {
            int ret = spawn_prog(*prog);
            if (ret != -1) {
                prog->init_prog_log();
            }
            return ret;
        }
        return 1;
    }

    int save_proc_to_cfg() {
        //name:cwd:command
        const std::string &cfg_path = this->pm_tiny_cfg_file;
        const std::string &app_log_dir = this->pm_tiny_app_log_dir;
        std::stringstream ss;
        std::vector<std::tuple<std::string, std::string>> f_envs;
        std::for_each(this->pm_tiny_progs.begin(), this->pm_tiny_progs.end(),
                      [&ss, &f_envs](const prog_ptr_t &p) {
                          std::string command = std::accumulate(p->args.begin(), p->args.end(),
                                                                std::string(""),
                                                                [](const std::string &s1, const std::string &s2) {
                                                                    return s1 + (s2 + " ");
                                                                });
                          mgr::utils::trim(command);
                          ss << p->name << ":" << p->work_dir << ":" << command << "\n";
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

    void restart_startfailed() {
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

    void spawn() {
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

    void close_fds() {
        for (auto &prog_info: pm_tiny_progs) {
            prog_info->close_fds();
        }
    }
};

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
    logger->safe_signal_log(sig);
    exit_signal = sig;
}

void sig_chld_handler(int sig, siginfo_t *info, void *ucontext) {
    logger->safe_signal_log(sig);
    exit_chld_signal = sig;
}

void sig_alarm_handler(int sig, siginfo_t *info, void *ucontext) {
    logger->safe_signal_log(sig);
    alarm_signal = sig;
}

void sig_hup_handler(int sig, siginfo_t *info, void *ucontext) {
    logger->safe_signal_log(sig);
    hup_signal = sig;
}

int64_t gettime_monotonic_ms() {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        logger->syscall_errorlog("clock_gettime");
    }
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


int spawn_prog(pm_tiny::prog_info_t &prog) {
    int pipefd[2];
    int pipefd2[2];
    volatile int failed = 0;
    int tmp_errno;
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

    pid_t pid = vfork();
    if (pid < 0) {
        tmp_errno = errno;
        logger->syscall_errorlog("vfork");
        errno = tmp_errno;
        return -1;
    }
    if (pid > 0) {
        close(pipefd[1]);
        close(pipefd2[1]);
        prog.pid = pid;
        prog.backup_pid = pid;
        prog.rpipefd[0] = pipefd[0];
        prog.rpipefd[1] = pipefd2[0];
        prog.last_startup_ms = gettime_monotonic_ms();
        int rc = pm_tiny::set_nonblock(pipefd[0]);
        if (rc == -1) {
            logger->syscall_errorlog("fcntl");
        }
        logger->info("startup %s pid:%d\n", prog.name.c_str(), pid);
    } else {
        close(pipefd[0]);
        close(pipefd2[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd2[1], STDERR_FILENO);
        close(pipefd[1]);
        close(pipefd2[1]);
        for (int i = getdtablesize(); i > 2; --i) {
            close(i);
        }
        int rc = pm_tiny::set_sigaction(SIGPIPE, SIG_DFL);
        if (rc == -1) {
            logger->syscall_errorlog("sigaction SIGPIPE");
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


pid_t wait_any_nohang(int *wstat) {
    return pm_tiny::safe_waitpid(-1, wstat, WNOHANG);
}


static int check_delayed_exit_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_exit_signal = exit_signal;
    exit_signal = 0;
    if (save_exit_signal) {
        bool terminate = save_exit_signal == SIGTERM
                         || save_exit_signal == SIGINT
                         || save_exit_signal == SIGSTOP;
        if (terminate) {
            kill_prog(pm_tiny_progs);
            /*bool find = false;
            for (auto &p: pm_tiny_progs) {
                if (p->pid != -1) {
                    int rt = kill(p->pid, save_exit_signal);
                    if (rt == -1) {
                        logger->syscall_errorlog("kill");
                    } else {
                        find = true;
                        p->pendding_signal = save_exit_signal;
                        p->state = PM_TINY_PROG_STATE_REQUEST_STOP;
                    }
                }
            }
            if (find) {
                auto rc = alarm(1);
                if (rc == -1) {
                    logger->syscall_errorlog("alarm");
                }
            }*/
        }
    }
    return save_exit_signal;
}

static void check_delayed_alarm_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_alarm_signal = alarm_signal;
    alarm_signal = 0;
    if (save_alarm_signal) {
        bool find = false;
        for (auto &p: pm_tiny_progs) {
            if (p->pid != -1 && p->pendding_signal != 0) {
                find = true;
                int rt = kill(p->pid, SIGKILL);
                if (rt == -1) {
                    logger->syscall_errorlog("kill SIGKILL");
                } else {
                    p->pendding_signal = SIGKILL;
                }
            }
        }
        if (find) {
            auto rc = alarm(2);
            if (rc == -1) {
                logger->syscall_errorlog("alarm");
            }
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
            logger->syscall_errorlog("kill");
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
        pm_tiny::safe_sleep(1);
        find = xx_wait_1(prog, WNOHANG);
        if (find) {
            pm_tiny::safe_sleep(1);
            find = xx_wait_1(prog, WNOHANG);
            if (find) {
                xx_kill_1(prog, SIGKILL);
                xx_wait_1(prog, 0);
            }
        }
    }
    prog->close_fds();
}

void kill_prog(proglist_t &progs) {
    auto xx_kill = [](proglist_t &progs, int signo) {
        bool find = false;
        for (auto iter = std::begin(progs); iter != std::end(progs); iter++) {
            auto p = *iter;
            find = xx_kill_1(p, signo);
        }
        return find;
    };
    auto xx_wait = [](proglist_t &progs, int options) {
        bool find = false;
        for (auto iter = std::begin(progs); iter != std::end(progs); iter++) {
            auto p = *iter;
            find = xx_wait_1(p, options);
        }
        return find;
    };
    bool find = xx_kill(progs, SIGTERM);
    if (find) {
        pm_tiny::safe_sleep(1);
        find = xx_wait(progs, WNOHANG);
        if (find) {
            pm_tiny::safe_sleep(1);
            find = xx_wait(progs, WNOHANG);
            if (find) {
                logger->debug("force kill");
                xx_kill(progs, SIGKILL);
                xx_wait(progs, 0);
            }
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
    for (auto it = pm_tiny_progs.begin(); it != pm_tiny_progs.end(); it++) {
        ss << "--- " << i++ << " ---\n";
        ss << *((*it).get()) << "\n";
    }
    logger->debug(ss.str().c_str());
}

static void check_delayed_hup_sig(pm_tiny_server_t &tiny_server) {
    proglist_t &pm_tiny_progs = tiny_server.pm_tiny_progs;
    sig_atomic_t save_hup_signal = hup_signal;
    hup_signal = 0;
    if (save_hup_signal) {
        proglist_t new_proglist;
        tiny_server.parse_cfg(new_proglist);
        proglist_t add_progs;
        proglist_t remove_progs;
        get_add_remove_progs(pm_tiny_progs, new_proglist, add_progs, remove_progs);
        logger->debug("prog add:%d remove:%d", add_progs.size(), remove_progs.size());
        kill_prog(remove_progs);
        logger->debug("--- erase before ---");
        log_proglist(pm_tiny_progs);
        pm_tiny_progs.erase(
                std::remove_if(pm_tiny_progs.begin(), pm_tiny_progs.end(), [&remove_progs](const prog_ptr_t &prog) {
                    return std::find(remove_progs.begin(), remove_progs.end(), prog) != remove_progs.end();
                }), pm_tiny_progs.end());
        logger->debug("--- erase after ---");
        log_proglist(pm_tiny_progs);
        tiny_server.restart_startfailed();
        std::for_each(std::begin(add_progs), std::end(add_progs), [](prog_ptr_t &prog) {
            int ret = spawn_prog(*prog);
            if (ret != -1) {
                prog->init_prog_log();
            }
        });
        std::copy(add_progs.begin(), add_progs.end(), std::back_inserter(pm_tiny_progs));
        logger->debug("--- add after ---");
        log_proglist(pm_tiny_progs);
    }
}

void log_proc_exit_status(pm_tiny::prog_info_t *prog, int pid, int wstatus) {
    const char *prog_name = "Unkown";
    float run_time = NAN;
    int restart_count = 0;
    if (prog) {
        prog_name = (char *) prog->name.c_str();
        run_time = (float) (gettime_monotonic_ms() - prog->last_startup_ms) / (60 * 1000.0f);
        restart_count = prog->dead_count;
    }
    if (WIFEXITED(wstatus)) {
        int exit_code = WEXITSTATUS(wstatus);
        logger->info("pid:%d name:%s exited, exit code %d run time:%.3f minutes restart:%d\n",
                     pid, prog_name, exit_code, run_time, restart_count);
    } else if (WIFSIGNALED(wstatus)) {
        int kill_signo = WTERMSIG(wstatus);
        char buf[80] = {0};
        mgr::utils::signal::signo_to_str(kill_signo, buf, false);
        logger->info("pid:%d name:%s killed by signal %s run time:%.3f minutes restart:%d\n",
                     pid, prog_name, buf, run_time, restart_count);
    }
}

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
                    auto now_ms = gettime_monotonic_ms();
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
                            int ret = pm_tiny_server_t::start_prog(p);
                            if (ret == -1) {
                                logger->syscall_errorlog("chld_sig try restart `%s` fail", p->name.c_str());
                            }
                        } else {
                            if (life_time > p->min_lifetime_threshold) {
                                logger->info("%s die %d times within %.4f minutes\n ", p->name.c_str(),
                                             p->dead_count_timer, p->moniter_duration_threshold / (60 * 1000.0f));
                            } else {
                                logger->info("life time %.4fs <= %.4fs\n", life_time / 1000.0f,
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

                size_t wait_proc_num = get_current_alive_prog(pm_tiny_progs);
                logger->info("current proc:%zu\n", wait_proc_num);
                if (wait_proc_num <= 0) {
                    break;
                }
            } else {
                if (rc == -1) {
                    logger->syscall_errorlog("waitpid");
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

int check_delayed_sigs(pm_tiny_server_t &tiny_server) {
    int ret = 0;
    check_delayed_hup_sig(tiny_server);
    ret = check_delayed_exit_sig(tiny_server);
    check_delayed_chld_sig(tiny_server);
    check_delayed_alarm_sig(tiny_server);
    return ret;
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

void install_alarm_handler() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_alarm_handler;
    sigaction(SIGALRM, &act, nullptr);
}

void install_hup_handler() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_hup_handler;
    sigaction(SIGHUP, &act, nullptr);
}

int open_uds_listen_fd(std::string sock_path) {
    int sfd;
    struct sockaddr_un my_addr;
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        logger->syscall_errorlog("socket");
        exit(EXIT_FAILURE);
    };

    memset(&my_addr, 0, sizeof(struct sockaddr_un));
    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, sock_path.c_str(),
            sizeof(my_addr.sun_path) - 1);

    int rc = pm_tiny::set_nonblock(sfd);
    if (rc < 0) {
        logger->syscall_errorlog("fcntl");
        exit(EXIT_FAILURE);
    }

    if (bind(sfd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) == -1) {
        logger->syscall_errorlog("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(sfd, 5) == -1) {
        logger->syscall_errorlog("listen");
        exit(EXIT_FAILURE);
    }
    return sfd;
}


void start(pm_tiny_server_t &pm_tiny_server) {
    logger->debug("pm_tiny pid:%d\n", getpid());
    fd_set rfds;
    fd_set wfds;
    int rc = 0;
    auto sock_path = pm_tiny_server.pm_tiny_home_dir + "/pm_tinyd.sock";
    unlink(sock_path.c_str());

    rc = pm_tiny::set_sigaction(SIGPIPE, SIG_IGN);
    if (rc == -1) {
        logger->syscall_errorlog("sigaction SIGPIPE");
    }
    int sock_fd = open_uds_listen_fd(sock_path);
    pm_tiny_server.parse_cfg();
    if (pm_tiny_server.pm_tiny_progs.empty()) {
        logger->debug("progs empty\n");
    }

    install_exit_handler();
    install_chld_handler();
    install_alarm_handler();
    install_hup_handler();
    pm_tiny_server.spawn();
    proglist_t &pm_tiny_progs = pm_tiny_server.pm_tiny_progs;
    int server_exit = 0;
    std::vector<pm_tiny::session_ptr_t> sessions;
    using pm_tiny::operator<<;
    while (true) {
        rc = check_delayed_sigs(pm_tiny_server);
        if (rc) {
            server_exit = rc;
        }
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
        if (!server_exit) {
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
            if (!server_exit) {
                pause();
                continue;
            } else {
                break;
            }
        }
        rc = select(max_fd + 1, &rfds, &wfds, nullptr, nullptr);
        if (rc == -1) {
            if (errno == EINTR) {
                continue;
            }
            logger->syscall_errorlog("select");
            break;
        }
        int _readyfd = 0;
        check_prog_has_event(rc, pm_tiny_progs, rfds, _readyfd);
        if (_readyfd < rc && !server_exit) {
            check_listen_sock_has_event(sock_fd, sessions, _readyfd, rfds);
            if (_readyfd < rc) {
                check_sock_has_event(rc, pm_tiny_server, _readyfd, rfds, wfds,
                                     sessions);
            }
        }
    }
    check_delayed_sigs(pm_tiny_server);
    size_t proc_num = get_current_alive_prog(pm_tiny_progs);
    while (proc_num > 0) {
        pause();
        check_delayed_sigs(pm_tiny_server);
        proc_num = get_current_alive_prog(pm_tiny_progs);
    }
    pm_tiny_server.close_fds();
    unlink(sock_path.c_str());
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
                logger->debug("%d is readable\n", s_fd);
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
                        if (is_alive) {
                            kill_prog(*iter);
                        }
                        if (is_alive) {
                            (*iter)->dead_count++;
                        }
                        int rc = pm_tiny_server_t::start_prog(*iter);
                        if (rc == -1) {
                            std::string errmsg(strerror(errno));
                            pm_tiny::fappend_value<int>(*wf, 1);
                            pm_tiny::fappend_value(*wf, errmsg);
                        } else {
                            pm_tiny::fappend_value<int>(*wf, 0);
                            pm_tiny::fappend_value(*wf, "success");
                        }
                    }
                    session->write_frame(wf);
                } else {
                    logger->info("unkown framae type:%#02X", f_type);
                }
            } else {
                //ignore
            }
            if (session->is_close()) {
                closed_session = true;
                logger->debug("%d is closed\n", s_fd);
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
    ifs >> name >> cwd >> command >> local_resolved >> env_num;
    std::vector<std::string> envs;
    envs.resize(env_num);
    for (int k = 0; k < env_num; k++) {
        ifs >> envs[k];
    }
//   std::cout << "name:`" + name << "` cwd:`" << cwd << "` command:`" << command
//   << "` local_resolved:" << local_resolved << std::endl;
    auto iter = std::find_if(pm_tiny_progs.begin(), pm_tiny_progs.end(),
                             [&name](const prog_ptr_t &prog) {
                                 return prog->name == name;
                             });
    auto wf = std::make_shared<pm_tiny::frame_t>();
    if (iter == pm_tiny_progs.end()) {
        const prog_ptr_t prog = pm_tiny_server.create_prog(name, cwd, command, envs);
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
            const prog_ptr_t prog = pm_tiny_server.create_prog(name, cwd, command, envs);
            if (local_resolved && prog
                && (prog->work_dir != _p->work_dir || prog->args != _p->args)) {
                pm_tiny::fappend_value<int>(*wf, 4);
                pm_tiny::fappend_value(*wf, "The cwd or command or environ has changed,"
                                            " please run the delete operation first");
            } else {
                _p->envs = envs;
                int rc = pm_tiny_server_t::start_prog(_p);
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
    //n pid:name:workdir:command:restart_count:state
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
        logger->debug("accept fd:%d\n", cfd);
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
    command_args args;
    int rc = 0;
    int exists = 0;
    parse_command_args(argc, argv, args);
    char cfg_path[PATH_MAX] = {0};
    char home_path[PATH_MAX] = {0};
    if (!args.cfg_file.empty()) {
        if (realpath(args.cfg_file.c_str(), cfg_path) == NULL) {
            pm_tiny::logger_stderr.syscall_errorlog("%s realpath", args.cfg_file.c_str());
            exit(EXIT_FAILURE);
        }
    }
    if (!args.home_dir.empty()) {
        if (realpath(args.home_dir.c_str(), home_path) == NULL) {
            pm_tiny::logger_stderr.syscall_errorlog("%s realpath", args.home_dir.c_str());
            exit(EXIT_FAILURE);
        }
    }
    std::string pm_tiny_home_dir = pm_tiny::get_pm_tiny_home_dir(home_path);
    std::string pm_lock_file = pm_tiny_home_dir + "/" + "pm_tiny.pid";
    exists = pm_tiny::is_directory_exists(pm_tiny_home_dir.c_str());
    pm_tiny::logger_stdout.debug("pm_tiny home:%s", pm_tiny_home_dir.c_str());
    if (exists == -1) {
        perror("is_directory_exists");
        exit(EXIT_FAILURE);
    }
    if (!exists) {
        rc = mkdir(pm_tiny_home_dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (rc == -1) {
            pm_tiny::logger_stderr.syscall_errorlog("mkdir %s", pm_tiny_home_dir.c_str());
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
            perror("is_directory_exists");
            exit(EXIT_FAILURE);
        }
        if (!exists) {
            int rc = mkdir(dir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            if (rc == -1) {
                pm_tiny::logger_stderr.syscall_errorlog("mkdir %s", dir.c_str());
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
    logger = std::make_shared<pm_tiny::logger_t>(pm_tiny_log_file.c_str());

    if (args.daemon) {
        daemonize();
    }
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