#include <sys/resource.h>
#include <sys/syscall.h>
#include <errno.h>

int getrlimit(int resource, struct rlimit *rlim)
{
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    if (resource != RLIMIT_NOFILE) {
        errno = EINVAL;
        return -1;
    }

    struct os01_rlimit64 k_rlim;
    int64_t ret = syscall2(SYS_getrlimit, (uint64_t)resource, (uint64_t)&k_rlim);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    rlim->rlim_cur = (unsigned long)k_rlim.cur;
    rlim->rlim_max = (unsigned long)k_rlim.max;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }
    if (resource != RLIMIT_NOFILE) {
        errno = EINVAL;
        return -1;
    }

    struct os01_rlimit64 k_rlim;
    k_rlim.cur = (uint64_t)rlim->rlim_cur;
    k_rlim.max = (uint64_t)rlim->rlim_max;

    int64_t ret = syscall2(SYS_setrlimit, (uint64_t)resource, (uint64_t)&k_rlim);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    return 0;
}
