//
// Created by qianlinluo@foxmail.com on 2022/7/27.
//

#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int i=0;
    for (;;) {
        printf("%s ===%d===\n", argv[0], i);
        i++;
        sleep(1);
    }
    return 0;
}