//
// Created by qianlinluo@foxmail.com on 2022/6/28.
//
#include "time_util.h"
#include <chrono>

namespace pm_tiny {
    namespace time {
        CElapsedTimer::CElapsedTimer(void) {
            reset();
        }

        CElapsedTimer::~CElapsedTimer(void) {
        }

        void CElapsedTimer::reset(void) {
            m_begin = std::chrono::steady_clock::now();
        }

        uint32_t CElapsedTimer::hh(void) const {
            using namespace std::chrono;
            return duration_cast<std::chrono::hours>(steady_clock::now() - m_begin).count();
        }

        uint32_t CElapsedTimer::mm(void) const {
            using namespace std::chrono;
            return duration_cast<minutes>(steady_clock::now() - m_begin).count();
        }

        uint32_t CElapsedTimer::sec(void) const {
            using namespace std::chrono;
            return duration_cast<seconds>(steady_clock::now() - m_begin).count();
        }

        uint32_t CElapsedTimer::ms(void) const {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now() - m_begin).count();
        }

        uint64_t CElapsedTimer::us(void) const {
            using namespace std::chrono;
            return duration_cast<microseconds>(steady_clock::now() - m_begin).count();
        }

        uint64_t CElapsedTimer::ns(void) const {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now() - m_begin).count();
        }

        int64_t gettime_monotonic_ms() {
            struct timespec ts{};
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                perror("clock_gettime");
                return -1;
            }
            return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        }

        char *strftime_fmt(char *buf, unsigned len, time_t *tp, const char *fmt) {
            time_t t;
            if (!tp) {
                tp = &t;
                ::time(tp);
            }
            /* Returns pointer to NUL */
            return buf + strftime(buf, len, fmt, localtime(tp));
        }

        char *strftime_HHMMSS(char *buf, unsigned len, time_t *tp) {
            return strftime_fmt(buf, len, tp, "%H:%M:%S");
        }

        char *strftime_YYYYMMDDHHMMSS(char *buf, unsigned len, time_t *tp) {
            return strftime_fmt(buf, len, tp, "%Y-%m-%d %H:%M:%S");
        }
    }
}