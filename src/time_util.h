#ifndef PM_TINY_TIME_UTILS_H
#define PM_TINY_TIME_UTILS_H

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <chrono>

namespace pm_tiny {
    namespace time {
        class CElapsedTimer final
        {
        public:
            CElapsedTimer(void);
            ~CElapsedTimer(void);

            void reset(void);
            uint32_t hh(void)const;
            uint32_t mm(void)const;
            uint32_t sec(void)const;
            uint32_t ms(void)const;
            uint64_t us(void)const;
            uint64_t ns(void)const;

        private:
            std::chrono::steady_clock::time_point m_begin;
        };
        int64_t gettime_monotonic_ms();

        char *strftime_fmt(char *buf, unsigned len, time_t *tp, const char *fmt);

        char *strftime_HHMMSS(char *buf, unsigned len, time_t *tp);

        char *strftime_YYYYMMDDHHMMSS(char *buf, unsigned len, time_t *tp);
    }
}

#endif