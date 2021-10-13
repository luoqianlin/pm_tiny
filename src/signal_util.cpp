#include "signal_util.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <iostream>
#include <unistd.h>

namespace mgr {
    namespace utils {
        namespace signal {
#define  TO_STR(x) #x

/*
 * 1) SIGHUP	 2) SIGINT	 3) SIGQUIT	 4) SIGILL	 5) SIGTRAP
 6) SIGABRT	 7) SIGBUS	 8) SIGFPE	 9) SIGKILL	10) SIGUSR1
11) SIGSEGV	12) SIGUSR2	13) SIGPIPE	14) SIGALRM	15) SIGTERM
16) SIGSTKFLT	17) SIGCHLD	18) SIGCONT	19) SIGSTOP	20) SIGTSTP
21) SIGTTIN	22) SIGTTOU	23) SIGURG	24) SIGXCPU	25) SIGXFSZ
26) SIGVTALRM	27) SIGPROF	28) SIGWINCH	29) SIGIO	30) SIGPWR
31) SIGSYS	34) SIGRTMIN	35) SIGRTMIN+1	36) SIGRTMIN+2	37) SIGRTMIN+3
38) SIGRTMIN+4	39) SIGRTMIN+5	40) SIGRTMIN+6	41) SIGRTMIN+7	42) SIGRTMIN+8
43) SIGRTMIN+9	44) SIGRTMIN+10	45) SIGRTMIN+11	46) SIGRTMIN+12	47) SIGRTMIN+13
48) SIGRTMIN+14	49) SIGRTMIN+15	50) SIGRTMAX-14	51) SIGRTMAX-13	52) SIGRTMAX-12
53) SIGRTMAX-11	54) SIGRTMAX-10	55) SIGRTMAX-9	56) SIGRTMAX-8	57) SIGRTMAX-7
58) SIGRTMAX-6	59) SIGRTMAX-5	60) SIGRTMAX-4	61) SIGRTMAX-3	62) SIGRTMAX-2
63) SIGRTMAX-1	64) SIGRTMAX
 * */

#define SIGNAL_NO_TO_STR(sig_mcro, buf) \
case sig_mcro:\
    strcat(buf, #sig_mcro);                        \
break

            void int_to_str(int n, char *buffer) {
                char *buf = buffer;
                int _n = abs(n);
                int j, tmp, i = 0;
                while (_n > 0) {
                    buf[i++] = '0' + _n % 10;
                    _n = _n / 10;
                }
                if (n < 0) {
                    buf[i++] = '-';
                }
                for (j = 0; j < i / 2; j++) {
                    tmp = buf[j];
                    buf[j] = buf[i - 1 - j];
                    buf[i - 1 - j] = tmp;
                }
                buf[i] = 0;

            }

            void signo_to_str(int sig, char *buffer, bool new_line) {
                switch (sig) {
                    SIGNAL_NO_TO_STR(SIGHUP, buffer);
                    SIGNAL_NO_TO_STR(SIGINT, buffer);
                    SIGNAL_NO_TO_STR(SIGQUIT, buffer);
                    SIGNAL_NO_TO_STR(SIGILL, buffer);
                    SIGNAL_NO_TO_STR(SIGTRAP, buffer);
                    SIGNAL_NO_TO_STR(SIGABRT, buffer);
                    SIGNAL_NO_TO_STR(SIGBUS, buffer);
                    SIGNAL_NO_TO_STR(SIGFPE, buffer);
                    SIGNAL_NO_TO_STR(SIGKILL, buffer);
                    SIGNAL_NO_TO_STR(SIGUSR1, buffer);
                    SIGNAL_NO_TO_STR(SIGSEGV, buffer);
                    SIGNAL_NO_TO_STR(SIGUSR2, buffer);
                    SIGNAL_NO_TO_STR(SIGPIPE, buffer);
                    SIGNAL_NO_TO_STR(SIGALRM, buffer);
                    SIGNAL_NO_TO_STR(SIGTERM, buffer);
                    SIGNAL_NO_TO_STR(SIGSTKFLT, buffer);
                    SIGNAL_NO_TO_STR(SIGCHLD, buffer);
                    SIGNAL_NO_TO_STR(SIGCONT, buffer);
                    SIGNAL_NO_TO_STR(SIGSTOP, buffer);
                    SIGNAL_NO_TO_STR(SIGTSTP, buffer);
                    SIGNAL_NO_TO_STR(SIGTTIN, buffer);
                    SIGNAL_NO_TO_STR(SIGTTOU, buffer);
                    SIGNAL_NO_TO_STR(SIGURG, buffer);
                    SIGNAL_NO_TO_STR(SIGXCPU, buffer);
                    SIGNAL_NO_TO_STR(SIGXFSZ, buffer);
                    SIGNAL_NO_TO_STR(SIGVTALRM, buffer);
                    SIGNAL_NO_TO_STR(SIGPROF, buffer);
                    SIGNAL_NO_TO_STR(SIGWINCH, buffer);
                    SIGNAL_NO_TO_STR(SIGIO, buffer);
                    SIGNAL_NO_TO_STR(SIGPWR, buffer);
                    SIGNAL_NO_TO_STR(SIGSYS, buffer);
                    default:
                        strcat(buffer, "other");
                        break;
                }
                strcat(buffer, "(");
                char *buf = buffer + strlen(buffer);
                int_to_str(sig, buf);
                strcat(buf, ")");
                if (new_line) {
                    strcat(buf, "\n");
                }

            }

            void signal_log(int sig, char *buf) {
                strcat(buf, "pid:");
                int_to_str(getpid(), buf + strlen(buf));
                strcat(buf, " rev signal:");
                signo_to_str(sig, buf, true);
            }
        }
    }
}