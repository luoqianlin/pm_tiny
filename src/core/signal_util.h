#ifndef PM_TINY_SIGNAL_UTIL_H
#define PM_TINY_SIGNAL_UTIL_H
#include <signal.h>
namespace mgr {
    namespace utils {
        namespace signal {
            void int_to_str(int n, char *buffer);
            void signo_to_str(int sig, char *buffer, bool new_line = true);
            void signal_log(int sig,char*buf);
            /* SIG_BLOCK/SIG_UNBLOCK all signals: */
            int sigprocmask_allsigs(int how,sigset_t *oset);
            void bb_signals(int sigs, void (*f)(int));
        }
    }
}
#endif //PM_TINY_SIGNAL_UTIL_H
