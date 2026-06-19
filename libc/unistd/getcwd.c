#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int getcwd(char *buf, uint64_t size)
{
#if defined(__is_libk)
    (void)buf; (void)size;
    return -1;
#else
    int64_t ret = syscall(SYS_getcwd, (uint64_t)buf, size, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
