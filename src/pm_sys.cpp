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
#include <time.h>


namespace pm_tiny {
    ssize_t safe_read(int fd, void *buf, size_t nbytes) {
        ssize_t rc;
        do {
            rc = ::read(fd, buf, nbytes);
        } while (rc == -1 && errno == EINTR);
        return rc;
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
        flags |= O_NONBLOCK;
        return fcntl(fd, F_SETFL, flags);
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
            rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain);
            if (rc != 0 && errno == EINTR) {
                memcpy(&request, &remain, sizeof(remain));
            } else {
                break;
            }
        } while (true);
        return rc;
    }
}