#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

int msync(void *addr, size_t length, int flags)
{
#if defined(__is_libk)
    (void)addr;
    (void)length;
    (void)flags;
    return -1;
#else
    int64_t ret = syscall(SYS_msync, (uint64_t)addr,
                          (uint64_t)length, (uint64_t)flags);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
