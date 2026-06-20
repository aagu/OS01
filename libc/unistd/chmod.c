#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int chmod(const char *path, int mode)
{
#if defined(__is_libk)
    (void)path; (void)mode;
    return -1;
#else
    int64_t ret = syscall(SYS_chmod, (uint64_t)path, (uint64_t)mode, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
