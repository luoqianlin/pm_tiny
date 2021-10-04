//
// Created by qianlinluo@foxmail.com on 23-7-10.
//
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <vector>
#include <algorithm>
#include <poll.h>


#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                               } while (0)

int open_uds_listen_fd(const std::string &sock_path) {
    int sfd;
    struct sockaddr_un my_addr{};
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1) {
        errExit("socket");
    };

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, sock_path.c_str(),
            sizeof(my_addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) == -1) {
        errExit("bind");
    }

    if (listen(sfd, 5) == -1) {
        errExit("listen");
    }
    return sfd;
}

int main(int argc, char *argv[]) {
    char path[PATH_MAX] = "/home/sansi/test_poll.sock";
    unlink(path);
    auto sock_fd = open_uds_listen_fd(path);
    printf("listen fd:%d\n", sock_fd);
    std::vector<struct pollfd> pfds(1);
    pfds[0].events = POLLIN;
    pfds[0].fd = sock_fd;
    while (true) {
        int ready = poll(pfds.data(), pfds.size(), -1);
        if (ready == -1) {
            errExit("poll");
        }
        for (nfds_t j = 0; j < pfds.size(); j++) {
            if (pfds[j].revents != 0) {
                printf("  fd=%d; events: %s%s%s%s\n", pfds[j].fd,
                       (pfds[j].revents & POLLIN) ? "POLLIN " : "",
                       (pfds[j].revents & POLLOUT) ? "POLLOUT " : "",
                       (pfds[j].revents & POLLHUP) ? "POLLHUP " : "",
                       (pfds[j].revents & POLLERR) ? "POLLERR " : "");

                if (pfds[j].revents & POLLIN) {
                    if (pfds[j].fd == sock_fd) {
                        int cfd;
                        struct sockaddr_un peer_addr{};
                        socklen_t peer_addr_size = sizeof(struct sockaddr_un);
                        cfd = accept4(sock_fd, (struct sockaddr *) &peer_addr,
                                      &peer_addr_size, SOCK_NONBLOCK);
                        printf("accept fd:%d\n", cfd);
                        struct pollfd pfd{};
                        pfd.fd = cfd;
                        pfd.events = POLLIN | POLLOUT;
                        pfds.push_back(pfd);
                    } else {
                        char buf[4096];
                        auto s = ::read(pfds[j].fd, buf, sizeof(buf));
                        if (s == -1) { errExit("read"); }
                        if (s == 0) {
                            printf("read 0 bytes,closing fd %d\n", pfds[j].fd);
                            if (close(pfds[j].fd) == -1)
                                errExit("close");
                            pfds[j].fd = -1;
                        } else {
                            printf("    read %zd bytes: %.*s\n", s, (int) s, buf);
                        }
                    }
                } else if (pfds[j].revents & POLLOUT) {
                    sleep(1);
                    close(pfds[j].fd);
                    printf("close fd:%d\n", pfds[j].fd);
                    pfds[j].fd = -1;
                } else {                /* POLLERR | POLLHUP */
                    printf("    closing fd %d\n", pfds[j].fd);
                    if (close(pfds[j].fd) == -1)
                        errExit("close");
                    pfds[j].fd = -1;
                }
            }
        }
        pfds.erase(std::remove_if(pfds.begin(), pfds.end(),
                                  [](auto &pfd) { return pfd.fd == -1; }), pfds.end());
        printf("pfds size:%zu\n", pfds.size());
    }
    close(sock_fd);
    unlink(path);
    return 0;
}