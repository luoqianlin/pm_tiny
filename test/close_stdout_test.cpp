//
// Created by luo on 2021/10/15.
//
#include <stdio.h>
#include <unistd.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern char **environ;

int main(int argc, char *argv[]) {
    printf("===hello %s===\n",argv[0]);
    signal(SIGINT,SIG_IGN);
    signal(SIGTERM,SIG_IGN);
    signal(SIGHUP,SIG_IGN);
    int i=0;
    close(STDOUT_FILENO);
//    close(STDIN_FILENO);
//    close(STDERR_FILENO);
    while (true) {
        printf("===%d===\n",i);
        i++;
        sleep(1);
    }
    return 0;
}