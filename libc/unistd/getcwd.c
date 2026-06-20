#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

char *getcwd(char *buf, size_t size)
{
#if defined(__is_libk)
    (void)buf; (void)size;
    return NULL;
#else
    int64_t ret = syscall(SYS_getcwd, (uint64_t)buf, size, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return NULL;
    }
    return 0;
#endif
}
