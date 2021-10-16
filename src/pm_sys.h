//
// Created by luo on 2021/10/7.
//

#ifndef PM_TINY_PM_SYS_H
#define PM_TINY_PM_SYS_H

#include <sys/types.h>
#include <signal.h>
#include <functional>

namespace pm_tiny {

    ssize_t safe_read(int fd, void *buf, size_t nbytes);

    ssize_t safe_write(int fd, const void *buf, size_t n);

    pid_t safe_waitpid(pid_t pid, int *wstat, int options);

    int set_nonblock(int fd);

    int set_sigaction(int sig, sighandler_t sighandler);

    int is_directory_exists(const char *path);

    int safe_sleep(int second);

    void sleep_waitfor(int check_interval_ms,int check_count,
                             const std::function<bool()>& predicate);
    void sleep_waitfor(int second,const std::function<bool()>& predicate,int interval_ms=20);

    int is_process_exists(int pid);

    int safe_kill_process(int pid, int tolerance_time_sec = 1);

    // Read VmRSS from /proc/[pid]/statm and convert to kiB.
    // Returns the value (>= 0) or -errno on error.
    long long get_vm_rss_kib(int pid);
}

#endif //PM_TINY_PM_SYS_H
