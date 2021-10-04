//
// Created by qianlinluo@foxmail.com on 2022/7/27.
//

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <cassert>
int main(int argc, char *argv[]) {
    int num = 0x143'full;
    assert(num == 0x143 * 16 + 15);
    for (int i = 0; i < 50; i++) {
        printf("%s ===%d===\n", argv[0], i);
        printf("PM_TINY_APP_NAME:%s\n", getenv("PM_TINY_APP_NAME"));
        sleep(1);
    }
    return -1;
}