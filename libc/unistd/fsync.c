#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int fsync(int fd)
{
#if defined(__is_libk)
    (void)fd;
    return -1;
#else
    int64_t ret = syscall(SYS_fsync, (uint64_t)fd, 0, 0);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
