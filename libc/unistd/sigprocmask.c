#include <signal.h>

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    (void)how; (void)set; (void)old;
    return 0;
}
