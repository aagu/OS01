#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int pipe2(int fds[2], int flags)
{
#if defined(__is_libk)
    (void)fds; (void)flags;
    return -1;
#else
    int64_t ret = syscall2(SYS_pipe2, (uint64_t)fds, (uint64_t)(int64_t)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
