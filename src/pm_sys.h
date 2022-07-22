//
// Created by luo on 2021/10/7.
//

#ifndef PM_TINY_PM_SYS_H
#define PM_TINY_PM_SYS_H

#include <sys/types.h>
#include <signal.h>
#include <functional>
#include <climits>

namespace pm_tiny {
    struct pty_info {
        int master_fd;
        char slave_name[PATH_MAX];
    };
    struct passwd_t {
        std::string pw_name;        /* Username.  */
        std::string pw_passwd;        /* Password.  */
        uid_t pw_uid;        /* User ID.  */
        gid_t pw_gid;        /* Group ID.  */
        std::string pw_gecos;        /* Real name.  */
        std::string pw_dir;            /* Home directory.  */
        std::string pw_shell;        /* Shell program.  */
    };

    ssize_t safe_read(int fd, void *buf, size_t nbytes);

    ssize_t safe_write(int fd, const void *buf, size_t n);

    pid_t safe_waitpid(pid_t pid, int *wstat, int options);

    int set_nonblock(int fd);

    int set_sigaction(int sig, sighandler_t sighandler);

    int is_directory_exists(const char *path);

    int safe_sleep(int second);

    void sleep_waitfor(int check_interval_ms, int check_count,
                       const std::function<bool()> &predicate);

    void sleep_waitfor(int second, const std::function<bool()> &predicate, int interval_ms = 20);

    int is_process_exists(int pid);

    int safe_kill_process(int pid, int tolerance_time_sec = 1);

    // Read VmRSS from /proc/[pid]/statm and convert to kiB.
    // Returns the value (>= 0) or -errno on error.
    long long get_vm_rss_kib(int pid);

    int get_uid_from_username(const char *name, passwd_t &passwd);
    int create_pty(struct pty_info *p);
}

#endif //PM_TINY_PM_SYS_H
