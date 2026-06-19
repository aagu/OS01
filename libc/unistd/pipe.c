#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int pipe(int fds[2])
{
#if defined(__is_libk)
    (void)fds;
    return -1;
#else
    int64_t ret = syscall(SYS_pipe, (uint64_t)fds, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
