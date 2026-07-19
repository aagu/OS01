#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

int fstat(int fd, struct stat *buf)
{
    int64_t ret = syscall(SYS_fstat, (uint64_t)fd, (uint64_t)buf, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}
