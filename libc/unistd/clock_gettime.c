#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>

int clock_gettime(int clk_id, struct timespec *tp)
{
#if defined(__is_libk)
    (void)clk_id; (void)tp;
    return -1;
#else
    int64_t ret = syscall(SYS_clock_gettime, (uint64_t)clk_id, (uint64_t)tp, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
