#include <signal.h>
#include <sys/syscall.h>

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    struct sigaction local_act;
    if (act) {
        local_act = *act;
        if (!local_act.sa_restorer)
            local_act.sa_restorer = sigreturn_trampoline;
        act = &local_act;
    }
    return (int)syscall(SYS_signal, (uint64_t)signum,
                        (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)oldact);
}
