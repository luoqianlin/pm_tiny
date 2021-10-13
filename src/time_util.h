#ifndef PM_TINY_TIME_UTILS_H
#define PM_TINY_TIME_UTILS_H

#include <stdio.h>
#include <time.h>


inline unsigned long long current_millisecond() {

    struct timespec start;
    int ret = clock_gettime(CLOCK_MONOTONIC, &start);
    if (ret != 0) {
        perror("clock_gettime");
    }
    unsigned long long _start = (start.tv_nsec + start.tv_sec * 1e9) / 1e6;
    return _start;
}


inline char *strftime_fmt(char *buf, unsigned len, time_t *tp, const char *fmt) {
    time_t t;
    if (!tp) {
        tp = &t;
        time(tp);
    }
    /* Returns pointer to NUL */
    return buf + strftime(buf, len, fmt, localtime(tp));
}

inline char *strftime_HHMMSS(char *buf, unsigned len, time_t *tp) {
    return strftime_fmt(buf, len, tp, "%H:%M:%S");
}

inline char *strftime_YYYYMMDDHHMMSS(char *buf, unsigned len, time_t *tp) {
    return strftime_fmt(buf, len, tp, "%Y-%m-%d %H:%M:%S");
}

#endif