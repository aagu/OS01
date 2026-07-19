#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

int access(const char *path, int mode)
{
    int64_t ret = syscall(SYS_access, (uint64_t)path, (uint64_t)mode, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}
