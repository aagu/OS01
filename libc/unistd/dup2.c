#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int dup2(int oldfd, int newfd)
{
#if defined(__is_libk)
    (void)oldfd; (void)newfd;
    return -1;
#else
    int64_t ret = syscall(SYS_dup2, (uint64_t)oldfd, (uint64_t)newfd, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
