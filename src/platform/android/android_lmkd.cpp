#include "android_lmkd.h"

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <pwd.h>
#include <memory>
#include <string>
#include <system_error>
#include <arpa/inet.h>
#include "daemon_log.h"
#include "pm_sys.h"

namespace pm_tiny {
#ifdef  __ANDROID__
    CloseableFd connect_lmkd() {
        const char *socket_path = "/dev/socket/lmkd";
        int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (sock < 0)
            throw std::system_error(errno, std::generic_category(), "socket");
        CloseableFd socket_fd(sock);
        struct sockaddr_un addr{};
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

        int ret = connect(socket_fd.fd_, (struct sockaddr *) &addr, sizeof(addr));
        if (ret < 0)
            throw std::system_error(errno, std::generic_category(), "connect");
        return socket_fd;
    }

    int lmk_procprio(int sock,pid_t pid,uid_t uid,int oom_score_adj) {
        int32_t msg[5];
        msg[0] = static_cast<int32_t>(lmk_cmd::LMK_PROCPRIO);
        msg[1] = pid;     // 进程ID
        msg[2] = static_cast<int32_t>(uid);     // 用户ID
        msg[3] = oom_score_adj; // oom_score_adj值
        msg[4] = static_cast<int32_t>(proc_type::PROC_TYPE_APP);
        for (int32_t & i : msg) {
            i = htonl(i);
        }
         int ret = static_cast<int>(safe_send(sock, msg, sizeof(msg),MSG_NOSIGNAL));
        if (ret < 0) {
            return ret;
        }
        return 0;
    }

    int cmd_procremove(int sock, pid_t pid) {
        uint32_t remove_msg[2];
        remove_msg[0] = static_cast<uint32_t>(lmk_cmd::LMK_PROCREMOVE);
        remove_msg[1] = pid;
        for (unsigned int &i: remove_msg) {
            i = htonl(i);
        }
        ssize_t ret = safe_send(sock, remove_msg, sizeof(remove_msg),MSG_NOSIGNAL);
        if (ret < 0) {
            return ret;
        }
        return 0;
    }
#else

    CloseableFd connect_lmkd() {
        return CloseableFd{-1};
    }

    int lmk_procprio(int sock, pid_t pid, uid_t uid, int oom_score_adj) {
        (void)sock;
        (void)pid;
        (void)uid;
        (void)oom_score_adj;
        return 0;
    }

    int cmd_procremove(int sock, pid_t pid) {
        (void)sock;
        (void)pid;
        return 0;
    }
#endif
}
