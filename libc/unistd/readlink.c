#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <errno.h>

ssize_t readlink(const char *path, char *buf, size_t bufsize)
{
    int64_t ret = syscall3(SYS_readlink, (uint64_t)path, (uint64_t)buf,
                           (uint64_t)bufsize);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
}
