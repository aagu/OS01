#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int64_t getppid(void)
{
#if defined(__is_libk)
    return 0;
#else
    int64_t ret = syscall(SYS_getppid, 0, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
#endif
}
