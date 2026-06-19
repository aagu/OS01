#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int64_t waitpid(int64_t pid, int *status, int options)
{
#if defined(__is_libk)
    (void)pid; (void)status; (void)options;
    return -1;
#else
    int64_t ret = syscall(SYS_waitpid, (uint64_t)pid, (uint64_t)status, (uint64_t)options);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
#endif
}
