//
// Created by qianlinluo@foxmail.com on 24-7-31.
//
//
// Created by qianlinluo@foxmail.com on 24-7-23.
//
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <memory>
#include <iostream>
#include <string>
#include <numeric>
#include <getopt.h>
#include <arpa/inet.h>
#include <array>

int main() {
    size_t memesize = 1 << 20;
    while (1) {
        printf("alloc %.3fMB\n", memesize / (1024 * 1024.0));
        auto addr = malloc(memesize);
        memset(addr, 0, memesize);
        sleep(1);
        memesize *= 2;
    }
}
