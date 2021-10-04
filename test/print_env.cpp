//
// Created by luo on 2021/10/15.
//
#include <stdio.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char *argv[]) {
    fprintf(stderr, "---print before---\n\n");
    for (char **env = environ; *env != nullptr; env++) {
        char *thisEnv = *env;
        fprintf(stderr, "%s\n", thisEnv);
    }
    fprintf(stderr, "---print after---\n\n");
    return 0;
}