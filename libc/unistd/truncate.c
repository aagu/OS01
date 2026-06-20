#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int truncate(const char *path, int64_t length)
{
#if defined(__is_libk)
    (void)path; (void)length;
    return -1;
#else
    int64_t ret = syscall(SYS_truncate, (uint64_t)path, (uint64_t)length, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
