#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int64_t write(int fd, const void *buf, uint64_t size)
{
#if defined(__is_libk)
    (void)fd; (void)buf; (void)size;
    return -1;
#else
    int64_t ret = syscall(SYS_write, (uint64_t)fd, (uint64_t)buf, size);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
#endif
}
