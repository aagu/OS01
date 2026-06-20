#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>

int kill(int64_t pid, int sig)
{
#if defined(__is_libk)
    (void)pid; (void)sig;
    return -1;
#else
    int64_t ret = syscall(SYS_kill, (uint64_t)pid, (uint64_t)sig, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
