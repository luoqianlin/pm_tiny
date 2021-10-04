//
// Created by qianlinluo@foxmail.com on 23-7-25.
//
#include<stdio.h>
#include <stdlib.h>

template<typename T>
char *get_value(T &v, char *buf) {
    puts(__PRETTY_FUNCTION__);
    printf("buf:%p\n", buf);
    v = *((T *) (buf));
    return buf + sizeof(T);
}

int main() {
    long long sz;
    char *buf = (char *) malloc(2 << 20);
    for (int i = 0; i < (2 << 20); i++) {
        buf[i] = rand() % 0xFF;
    }
    auto ptr = buf + (1 << 20) + 13;
    printf("ptr:%p\n", ptr);
    ptr = get_value(sz, ptr);
//    printf("buf:%p,sz:%p,sz:%#llx\n", buf, &sz, sz);
    char cv;
    ptr = get_value(cv, ptr);
//    printf("cv:%d\n", cv);
    long long dv;
    for (int i = 0; i < 2000; i++) {
        get_value(dv, ptr);
        ptr += 1;
    }
//    printf("dv:%f\n", dv);
    printf("%llx,%d,%llx\n", sz, cv, *(reinterpret_cast<long long *>(&dv)));
    free(buf);
    printf("\n====end===\n");
    return 0;
}