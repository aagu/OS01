#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int fchmod(int fd, int mode)
{
#if defined(__is_libk)
    (void)fd; (void)mode;
    return -1;
#else
    int64_t ret = syscall(SYS_fchmod, (uint64_t)fd, (uint64_t)mode, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
