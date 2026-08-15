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
#include <dirent.h>
#include <stdio.h>
#include <algorithm>


#include "daemon_log.h"
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

    namespace {
        bool read_process_state(int pid, char &state) {
            char path[256] = {0};
            snprintf(path, sizeof(path), "%s/%d/stat", procdir_path, pid);
            FILE *file = fopen(path, "r");
            if (file == nullptr) return false;
            char buffer[4096] = {0};
            const bool read_ok = fgets(buffer, sizeof(buffer), file) != nullptr;
            fclose(file);
            if (!read_ok) return false;
            const char *comm_end = strrchr(buffer, ')');
            if (comm_end == nullptr || comm_end[1] != ' ' || comm_end[2] == '\0') return false;
            state = comm_end[2];
            return true;
        }
    }

    int is_process_exists(int pid) {
        if (pid <= 0) return 0;
        int rc = kill(pid, 0);
        if (rc == -1) {
            if (errno == ESRCH) {
                errno = 0;
                return 0;
            }
        }
        char state = '\0';
        if (read_process_state(pid, state) &&
            (state == 'Z' || state == 'X' || state == 'x')) {
            return 0;
        }
        return 1;
    }

    process_wait_result wait_for_process_exit(
            int pid, int timeout_ms, int check_interval_ms,
            const std::function<bool()> &interrupted) {
        if (!is_process_exists(pid)) return process_wait_result::exited;
        if (timeout_ms <= 0) return process_wait_result::timed_out;
        if (check_interval_ms <= 0) check_interval_ms = 100;
        const int64_t deadline = time::gettime_monotonic_ms() + timeout_ms;
        while (is_process_exists(pid)) {
            if (interrupted && interrupted()) return process_wait_result::interrupted;
            const int64_t now = time::gettime_monotonic_ms();
            if (now >= deadline) return process_wait_result::timed_out;
            const int remaining = static_cast<int>(deadline - now);
            const int sleep_ms = std::min(check_interval_ms, remaining);
            struct timespec request{};
            request.tv_sec = sleep_ms / 1000;
            request.tv_nsec = static_cast<long>(sleep_ms % 1000) * 1000000L;
            while (nanosleep(&request, &request) == -1 && errno == EINTR) {
                if (interrupted && interrupted()) return process_wait_result::interrupted;
            }
        }
        return process_wait_result::exited;
    }

    int safe_kill_process(int pid, int tolerance_time) {
        int rc = kill(pid, SIGTERM);
        if (rc == -1) {
            PM_TINY_DLOG_ERROR_ERRNO("kill pid:%d", pid);
            return -1;
        }
        time::CElapsedTimer elapsedTimer;
        pm_tiny::sleep_waitfor(tolerance_time, [&pid]() {
            return !pm_tiny::is_process_exists(pid);
        });
        if (pm_tiny::is_process_exists(pid)) {
            PM_TINY_DLOG_INFO("pid:%d still exists,force kill", pid);
            rc = kill(pid, SIGKILL);
            if (rc == -1) {
                PM_TINY_DLOG_ERROR_ERRNO("force kill pid:%d", pid);
            }
        }
        auto kill_cost = elapsedTimer.sec();
        if (kill_cost > 1) {
            PM_TINY_DLOG_INFO("kill pid:%d cost:%us", pid, kill_cost);
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
                PM_TINY_DLOG_ERROR_ERRNO("could not read page size");
                exit(EXIT_FAILURE);
            }
        }

        // Convert to kiB
        vm_rss_kib = vm_rss_kib * page_size / 1024;
        return vm_rss_kib;
    }

    namespace {
    void copy_passwd(const struct passwd &pwd, passwd_t &passwd_) {
#define ASSIGN_CHECK_NULL(str) (str)==nullptr ? "":(str)
        passwd_.pw_dir = ASSIGN_CHECK_NULL(pwd.pw_dir);
        passwd_.pw_gecos = ASSIGN_CHECK_NULL(pwd.pw_gecos);
        passwd_.pw_gid = pwd.pw_gid;
        passwd_.pw_name = ASSIGN_CHECK_NULL(pwd.pw_name);
        passwd_.pw_passwd = ASSIGN_CHECK_NULL(pwd.pw_passwd);
        passwd_.pw_shell = ASSIGN_CHECK_NULL(pwd.pw_shell);
        passwd_.pw_uid = pwd.pw_uid;
#undef ASSIGN_CHECK_NULL
    }

    long passwd_buffer_size() {
        const auto configured = sysconf(_SC_GETPW_R_SIZE_MAX);
        return configured > 0 ? configured : 16384;
    }
    }

    int get_uid_from_username(const char *name, passwd_t &passwd_) {
        struct passwd pwd;
        struct passwd *result;
        int s;
        const auto bufsize = passwd_buffer_size();
        std::unique_ptr<char[]> buf(new char[bufsize]);
        errno = 0;
        s = getpwnam_r(name, &pwd, buf.get(), bufsize, &result);
        if (result == nullptr) {
            errno = s == 0 ? ENOENT : s;
            return -1;
        }

        //fix in android
        // pwd.pw_dir:/,pwd.pw_gecos:(null),pwd.pw_name:root,pwd.pw_passwd:(null),pwd.pw_shell:/bin/sh
        copy_passwd(pwd, passwd_);
        return 0;
    }

    int get_user_from_uid(uid_t uid, passwd_t &passwd_) {
        struct passwd pwd;
        struct passwd *result;
        const auto bufsize = passwd_buffer_size();
        std::unique_ptr<char[]> buf(new char[bufsize]);
        errno = 0;
        const int status = getpwuid_r(uid, &pwd, buf.get(), bufsize, &result);
        if (result == nullptr) {
            errno = status == 0 ? ENOENT : status;
            return -1;
        }
        copy_passwd(pwd, passwd_);
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

    void close_all_fds() {
        DIR *dir = opendir("/proc/self/fd");
        if (!dir) {
            perror("opendir");
            return;
        }

        struct dirent *entry;
        while ((entry = readdir(dir))) {
            int fd;
            // Attempt to convert the name to a file descriptor
            if (sscanf(entry->d_name, "%d", &fd) == 1) {
                if (fd != dirfd(dir)) { // Don't close the directory handle itself
                    close(fd);
                }
            }
        }

        closedir(dir);
    }
}
