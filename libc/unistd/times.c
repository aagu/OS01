#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/times.h>

uint64_t times(struct tms *buf)
{
#if defined(__is_libk)
    (void)buf;
    return 0;
#else
    int64_t ret = syscall(SYS_times, (uint64_t)buf, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return (uint64_t)-1;
    }
    return (uint64_t)ret;
#endif
}
