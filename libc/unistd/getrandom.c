#include <sys/random.h>
#include <errno.h>
#include <sys/syscall.h>

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
#if defined(__is_libk)
    (void)buf; (void)buflen; (void)flags;
    return -1;   // not implemented in kernel mode
#else
    int64_t ret = syscall(SYS_getrandom, (uint64_t)buf, (uint64_t)buflen,
                          (uint64_t)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
#endif
}
