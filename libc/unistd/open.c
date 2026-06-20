#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int open(const char *path, int flags, ...)
{
#if defined(__is_libk)
    (void)path; (void)flags;
    return -1;
#else
    int64_t ret = syscall(SYS_open, (uint64_t)path, (uint64_t)flags, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
