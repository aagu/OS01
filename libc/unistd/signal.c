#include <signal.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <stdint.h>

sighandler_t signal(int signum, sighandler_t handler)
{
#if defined(__is_libk)
    (void)signum; (void)handler;
    return SIG_ERR;
#else
    struct sigaction act, oldact;
    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_restorer = NULL;
    act.sa_mask = 0;

    int64_t ret = syscall(SYS_signal,
                          (uint64_t)signum,
                          (uint64_t)&act,
                          (uint64_t)&oldact);
    if (ret < 0) {
        return SIG_ERR;
    }
    return oldact.sa_handler;
#endif
}
