//
// Created by luo on 2021/10/7.
//

#include "pm_sys.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <pwd.h>
#include <stdlib.h>
#include <memory>


#include "log.h"
#include "time_util.h"
#include "globals.h"

namespace pm_tiny {
    ssize_t safe_read(int fd, void *buf, size_t nbytes) {
        ssize_t rc;
        do {
            rc = ::read(fd, buf, nbytes);
        } while (rc == -1 && errno == EINTR);
        return rc;
    }

    ssize_t safe_send(int fd, const void *buf, size_t n, int flags) {
        ssize_t nbytes;
        do {
            nbytes = ::send(fd, buf, n, flags);
        } while (nbytes == -1 && errno == EINTR);
        return nbytes;
    }

    ssize_t safe_write(int fd, const void *buf, size_t n) {
        ssize_t nbytes = 0;
        do {
            nbytes = ::write(fd, buf, n);
        } while (nbytes == -1 && errno == EINTR);
        return nbytes;
    }

    pid_t safe_waitpid(pid_t pid, int *wstat, int options) {
        pid_t r;

        do {
            r = waitpid(pid, wstat, options);
        } while ((r == -1) && (errno == EINTR));
        return r;
    }

    int set_nonblock(int fd) {
        int flags = fcntl(fd, F_GETFL);
        if (flags == -1) {
            return -1;
        }
        flags |= O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags);
    }

    int set_cloexec(int fd) {
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1) {
            return -1;
        }
        flags |= FD_CLOEXEC;
        return fcntl(fd, F_SETFD, flags);
    }

    int set_sigaction(int sig, sighandler_t sighandler) {
        struct sigaction sa;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = sighandler;
        return sigaction(sig, &sa, nullptr);
    }

    int is_directory_exists(const char *path) {
        if (access(path, F_OK) == 0) {
            struct stat st;
            int rc = stat(path, &st);
            if (rc == -1) {
                return rc;
            }
            if (S_ISDIR(st.st_mode)) {
                return 1;
            }
        }
        return 0;
    }

    int safe_sleep(int second) {
        struct timespec request, remain;
        int rc;
        request.tv_nsec = 0;
        request.tv_sec = second;
        memset(&remain, 0, sizeof(remain));
        do {
            errno = 0;
            rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain);
            if (rc == EINTR) {
                memcpy(&request, &remain, sizeof(remain));
            } else {
                break;
            }
        } while (true);
        return rc;
    }

    int sleep_waitfor_0(int check_interval_ms, const std::function<bool()> &predicate) {
        struct timespec request, remain;
        int rc;
        request.tv_nsec = check_interval_ms * 1000000;
        request.tv_sec = 0;
        memset(&remain, 0, sizeof(remain));
        bool pred = true;
        do {
            errno = 0;
            rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain);
            pred = predicate();
            if (rc == EINTR && !pred) {
                memcpy(&request, &remain, sizeof(remain));
            } else {
                break;
            }
        } while (true);
        return pred;
    }

    void sleep_waitfor(int check_interval_ms, int check_count,
                       const std::function<bool()> &predicate) {
        for (int i = 0; i < check_count; i++) {
            bool finish = sleep_waitfor_0(check_interval_ms, predicate);
            if (finish) {
                break;
            }
        }
    }

    void sleep_waitfor(int second, const std::function<bool()> &predicate, int interval_ms) {
        sleep_waitfor(interval_ms, second * 1000 / interval_ms, predicate);
    }

    int is_process_exists(int pid) {
        int rc = kill(pid, 0);
        if (rc == -1) {
            if (errno == ESRCH) {
                errno = 0;
                return 0;
            }
        }
        return 1;
    }

    int safe_kill_process(int pid, int tolerance_time) {
        int rc = kill(pid, SIGTERM);
        if (rc == -1) {
            PM_TINY_LOG_E_SYS("kill pid:%d", pid);
            return -1;
        }
        time::CElapsedTimer elapsedTimer;
        pm_tiny::sleep_waitfor(tolerance_time, [&pid]() {
            return !pm_tiny::is_process_exists(pid);
        });
        if (pm_tiny::is_process_exists(pid)) {
            PM_TINY_LOG_I("pid:%d still exists,force kill", pid);
            rc = kill(pid, SIGKILL);
            if (rc == -1) {
                PM_TINY_LOG_E_SYS("force kill pid:%d", pid);
            }
        }
        auto kill_cost = elapsedTimer.sec();
        if (kill_cost > 1) {
            PM_TINY_LOG_I("kill pid:%d cost:%us", pid, kill_cost);
        }
        return 0;
    }

    // Read VmRSS from /proc/[pid]/statm and convert to kiB.
    // Returns the value (>= 0) or -errno on error.
    long long get_vm_rss_kib(int pid) {

        long long vm_rss_kib = -1;
        char path[256] = {0};

        // Read VmRSS from /proc/[pid]/statm (in pages)
        snprintf(path, sizeof(path), "%s/%d/statm", procdir_path, pid);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            return -errno;
        }
        int matches = fscanf(f, "%*u %lld", &vm_rss_kib);
        fclose(f);
        if (matches < 1) {
            return -ENODATA;
        }

        // Read and cache page size
        static long page_size;
        if (page_size == 0) {
            page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0) {
                PM_TINY_LOG_E_SYS("could not read page size");
                exit(EXIT_FAILURE);
            }
        }

        // Convert to kiB
        vm_rss_kib = vm_rss_kib * page_size / 1024;
        return vm_rss_kib;
    }

    int get_uid_from_username(const char *name, passwd_t &passwd_) {
        struct passwd pwd;
        struct passwd *result;
        int s;
        auto bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1) {          /* Value was indeterminate */
            bufsize = 16384;        /* Should be more than enough */
        }
        std::unique_ptr<char[]> buf(new char[bufsize]);
        errno = 0;
        s = getpwnam_r(name, &pwd, buf.get(), bufsize, &result);
        if (result == nullptr) {
            if (s == 0) {
                errno = 0;
            } else {
                errno = s;
            }
            return -1;
        }
        passwd_.pw_dir = pwd.pw_dir;
        passwd_.pw_gecos = pwd.pw_gecos;
        passwd_.pw_gid = pwd.pw_gid;
        passwd_.pw_name = pwd.pw_name;
        passwd_.pw_passwd = pwd.pw_passwd;
        passwd_.pw_shell = pwd.pw_shell;
        passwd_.pw_uid = pwd.pw_uid;
        return 0;
    }


    int create_pty(struct pty_info *p) {
        errno = 0;

        do {
            if (p == nullptr) {
                errno = EINVAL;
                break;
            }

            p->master_fd = posix_openpt(O_RDWR);
            if (p->master_fd < 0) {
                perror("posix_openpt() failed");
                break;
            }
            if (grantpt(p->master_fd) != 0) {
                perror("grantpt() failed");
                break;
            }
            if (unlockpt(p->master_fd) != 0) {
                perror("unlockpt() failed");
                break;
            }

            if (ptsname_r(p->master_fd, p->slave_name, PATH_MAX) != 0) {
                perror("ptsname_r() failed");
                break;
            }
        } while (false);
        int failno = errno;
        if (failno && p && p->master_fd >= 0) {
            close(p->master_fd);
        }
        return failno;
    }

    int tcsetattr_stdin_TCSANOW(const struct ::termios *tp) {
        return tcsetattr(STDIN_FILENO, TCSANOW, tp);
    }

    void process_reboot() {

        if (!debug_mode) {
            /* Terminate all monitored processes */
            kill(0, SIGTERM);

            /* Terminate init, reboot the system */
            kill(1, SIGTERM);
        } else {
            const char msg[] = "pm_tiny: Reboot disabled in debug mode, exiting.\n";

            /* Display reboot message */
            ::write(STDERR_FILENO, msg, sizeof(msg));

            /* Exit abnormally */
//            exit(1);
            kill(getpid(),SIGTERM);
        }
    }
}