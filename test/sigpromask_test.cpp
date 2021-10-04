#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

void print(const char*str){
    write(STDOUT_FILENO,str, strlen(str));
}

void handle_sa_sigaction(int signo, siginfo_t *siginfo, void *context) {
    print("recv INT signal\n");
}
void handle_term_sigaction(int signo, siginfo_t *siginfo, void *context) {
    print("recv TERM signal\n");
}
int main(int argc, char *argv[]) {
    printf("pid:%d\n",getpid());
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = handle_sa_sigaction;
    sigaction(SIGINT, &act, nullptr);
    act.sa_sigaction=handle_term_sigaction;
    sigaction(SIGTERM, &act, nullptr);
    print("---pause---\n");
    pause();
    sigset_t sigset;
    sigfillset(&sigset);
    sigprocmask(SIG_BLOCK,&sigset, nullptr);
    print("---SIG_BLOCK---\n");
    sleep(10);
    sigprocmask(SIG_UNBLOCK,&sigset, nullptr);
    print("---SIG_UNBLOCK---\n");
    pause();
    print("---exit---\n");
    return 0;
}

