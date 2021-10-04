//
// Created by qianlinluo@foxmail.com on 23-7-10.
//
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <cstddef>
#include <sys/socket.h>
#include <sys/un.h>
#include <algorithm>
#include <poll.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                               } while (0)
int main() {
    struct sockaddr_un serun{};
    int len;
    int sockfd;

    auto sock_path = "/home/sansi/test_poll.sock";

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("client socket error");
        exit(1);
    }
    memset(&serun, 0, sizeof(serun));
    serun.sun_family = AF_UNIX;
    strcpy(serun.sun_path, sock_path);
    len = offsetof(struct sockaddr_un, sun_path) + strlen(serun.sun_path);
    if (connect(sockfd, (struct sockaddr *) &serun, len) < 0) {
        perror("connect error");
        exit(1);
    }
    int flags = fcntl(sockfd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, flags);
    struct pollfd pfd;
    pfd.events = POLLOUT | POLLIN;
    pfd.fd = sockfd;
    sleep(3);
    while (true) {
        auto ready = poll(&pfd, 1, -1);
        if (ready == -1) {
            errExit("poll");
        }
        printf("  fd=%d; events: %s%s%s%s\n", pfd.fd,
               (pfd.revents & POLLIN) ? "POLLIN " : "",
               (pfd.revents & POLLOUT) ? "POLLOUT " : "",
               (pfd.revents & POLLHUP) ? "POLLHUP " : "",
               (pfd.revents & POLLERR) ? "POLLERR " : "");
        if (pfd.revents & POLLIN) {
            char buf[10];
            auto rc = read(pfd.fd, buf, sizeof(buf));
            if (rc == 0) {
                printf("fd %d closed\n", pfd.fd);
            }
        }
        if (pfd.revents & POLLOUT) {
            write(pfd.fd, "abc", 3);
        }
        sleep(1);
    }
    close(sockfd);
    return 0;
}