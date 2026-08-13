#ifndef PM_TINY_TIME_UTILS_H
#define PM_TINY_TIME_UTILS_H

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <chrono>

#define TIME_THIS_TO(_tt_op,_tt_out)                                                                            \
    {                                                                                                           \
        auto _tt_start = std::chrono::high_resolution_clock::now();                                             \
        {_tt_op;}                                                                                               \
        auto _tt_stop = std::chrono::high_resolution_clock::now();                                              \
        auto _tt_thetime = _tt_stop-_tt_start;                                                                          \
        using std::chrono::duration_cast;                                                                       \
        using std::chrono::duration;                                                                            \
        if (_tt_thetime >= std::chrono::minutes(1))                                                             \
            _tt_out << "\ntime: " << duration_cast<duration<double,std::ratio<60>>>(_tt_thetime).count() << "min\n";           \
        else if (_tt_thetime >= std::chrono::seconds(1))                                                        \
            _tt_out << "\ntime: " << duration_cast<duration<double>>(_tt_thetime).count() << "sec\n";           \
        else if (_tt_thetime >= std::chrono::milliseconds(1))                                                   \
            _tt_out << "\ntime: " << duration_cast<duration<double,std::milli>>(_tt_thetime).count() << "ms\n"; \
        else if (_tt_thetime >= std::chrono::microseconds(1))                                                   \
            _tt_out << "\ntime: " << duration_cast<duration<double,std::micro>>(_tt_thetime).count() << "us\n"; \
        else                                                                                                    \
            _tt_out << "\ntime: " << duration_cast<duration<double,std::nano>>(_tt_thetime).count() << "ns\n";  \
    }

#define TIME_THIS(_tt_op)  TIME_THIS_TO(_tt_op,std::cout)

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