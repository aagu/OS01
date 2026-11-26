#include <sys/uio.h>
#include <errno.h>
#include <sys/syscall.h>

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
#if defined(__is_libk)
    (void)fd; (void)iov; (void)iovcnt;
    return -1;
#else
    int64_t ret = syscall(SYS_readv, (uint64_t)fd, (uint64_t)iov, (uint64_t)(int64_t)iovcnt);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
#endif
}
