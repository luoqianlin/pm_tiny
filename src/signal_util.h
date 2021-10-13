#ifndef PM_TINY_SIGNAL_UTIL_H
#define PM_TINY_SIGNAL_UTIL_H
namespace mgr {
    namespace utils {
        namespace signal {
            void int_to_str(int n, char *buffer);
            void signo_to_str(int sig, char *buffer, bool new_line = true);
            void signal_log(int sig,char*buf);
        }
    }
}
#endif //PM_TINY_SIGNAL_UTIL_H
