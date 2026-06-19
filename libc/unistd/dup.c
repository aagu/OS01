#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int dup(int oldfd)
{
#if defined(__is_libk)
    (void)oldfd;
    return -1;
#else
    int64_t ret = syscall(SYS_dup, (uint64_t)oldfd, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
