#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int close(int fd)
{
#if defined(__is_libk)
    (void)fd;
    return -1;
#else
    int64_t ret = syscall(SYS_close, (uint64_t)fd, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
