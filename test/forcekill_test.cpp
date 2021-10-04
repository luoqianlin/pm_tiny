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
    signal(SIGINT,SIG_IGN);
    signal(SIGTERM,SIG_IGN);
    signal(SIGHUP,SIG_IGN);
    int i=0;
    while (true) {
//        fprintf(stderr,"---print before---\n\n");
//        for (char **env = environ; *env != 0; env++) {
//            char *thisEnv = *env;
//            fprintf(stderr, "%s\n", thisEnv);
//        }
//        fprintf(stderr,"---print after---\n\n");
        printf("===%d===\n",i);
        i++;
        sleep(1);
    }
    return 0;
}