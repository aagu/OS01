#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

int stat(const char *path, struct stat *buf)
{
    int64_t ret = syscall(SYS_stat, (uint64_t)path, (uint64_t)buf, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}
