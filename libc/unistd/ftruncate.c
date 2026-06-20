#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int ftruncate(int fd, int64_t length)
{
#if defined(__is_libk)
    (void)fd; (void)length;
    return -1;
#else
    int64_t ret = syscall(SYS_ftruncate, (uint64_t)fd, (uint64_t)length, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
