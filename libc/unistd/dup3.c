#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int dup3(int oldfd, int newfd, int flags)
{
#if defined(__is_libk)
    (void)oldfd; (void)newfd; (void)flags;
    return -1;
#else
    int64_t ret = syscall(SYS_dup3, (uint64_t)oldfd, (uint64_t)newfd,
                          (uint64_t)(int64_t)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
