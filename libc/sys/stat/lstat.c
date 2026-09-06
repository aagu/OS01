#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <errno.h>

int lstat(const char *path, struct stat *buf)
{
    int64_t ret = syscall3(SYS_lstat, (uint64_t)path, (uint64_t)buf, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}
