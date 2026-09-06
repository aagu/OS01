#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <errno.h>

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    int64_t ret = syscall4(SYS_fstatat, dirfd, (uint64_t)path,
                           (uint64_t)buf, (uint64_t)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}
