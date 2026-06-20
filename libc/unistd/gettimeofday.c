#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
#if defined(__is_libk)
    (void)tv; (void)tz;
    return -1;
#else
    int64_t ret = syscall(SYS_gettimeofday, (uint64_t)tv, (uint64_t)tz, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
