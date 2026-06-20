#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/time.h>

int nanosleep(const struct timespec *req, struct timespec *rem)
{
#if defined(__is_libk)
    (void)req; (void)rem;
    return -1;
#else
    int64_t ret = syscall(SYS_nanosleep, (uint64_t)req, (uint64_t)rem, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
