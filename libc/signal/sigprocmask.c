#include <signal.h>
#include <sys/syscall.h>

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    return (int)syscall(SYS_sigprocmask, (uint64_t)how,
                        (uint64_t)(uintptr_t)set,
                        (uint64_t)(uintptr_t)old);
}
