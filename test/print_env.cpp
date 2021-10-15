//
// Created by luo on 2021/10/15.
//
#include <stdio.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char *argv[]) {
    while (true) {
        fprintf(stderr,"---print before---\n\n");
        for (char **env = environ; *env != 0; env++) {
            char *thisEnv = *env;
            fprintf(stderr, "%s\n", thisEnv);
        }
        fprintf(stderr,"---print after---\n\n");
        sleep(1);
    }
    return 0;
}