//
// Created by qianlinluo@foxmail.com on 23-7-13.
//
#include <unistd.h>
#include <wait.h>
#include <stdio.h>
#include <errno.h>

int main() {
    auto rc = fork();
    if (rc == 0) {
        printf("pid:%d\n", getpid());
    } else {
        printf("before parent pid:%d\n", getpid());
        sleep(5);
        printf("after parent pid:%d\n", getpid());
        auto ret=kill(rc,0);
        if(ret==-1){
            if (errno == ESRCH) {
                printf("processs not exists\n");
            }
        }
        auto pid = waitpid(rc, nullptr, 0);
        if (pid == rc) {
            printf("waitpid success\n");
        }
    }
    return 0;
}