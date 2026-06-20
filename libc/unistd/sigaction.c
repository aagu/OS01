#include <signal.h>
#include <sys/syscall.h>

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return (int)syscall(SYS_signal, (uint64_t)signum, (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)oldact);
}
