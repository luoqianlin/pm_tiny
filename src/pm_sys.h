//
// Created by luo on 2021/10/7.
//

#ifndef PM_TINY_PM_SYS_H
#define PM_TINY_PM_SYS_H

#include <sys/types.h>
#include <signal.h>

namespace pm_tiny {
    ssize_t safe_read(int fd, void *buf, size_t nbytes);

    ssize_t safe_write(int fd, const void *buf, size_t n);

    pid_t safe_waitpid(pid_t pid, int *wstat, int options);

    int set_nonblock(int fd);

    int set_sigaction(int sig, sighandler_t sighandler);

    int is_directory_exists(const char *path);
    int safe_sleep(int second);
}

#endif //PM_TINY_PM_SYS_H
