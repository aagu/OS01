#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int unlink(const char *path)
{
#if defined(__is_libk)
    (void)path;
    return -1;
#else
    int64_t ret = syscall(SYS_unlink, (uint64_t)path, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
