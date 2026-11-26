#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/syscall.h>

int openat(int dirfd, const char *path, int flags, ...)
{
#if defined(__is_libk)
    (void)dirfd; (void)path; (void)flags;
    return -1;
#else
    uint64_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (uint64_t)va_arg(ap, int);
        va_end(ap);
    }
    int64_t ret = syscall4(SYS_openat, (uint64_t)(int64_t)dirfd,
                           (uint64_t)path, (uint64_t)(int64_t)flags, mode);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
