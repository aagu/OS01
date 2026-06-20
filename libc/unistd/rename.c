#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int rename(const char *oldpath, const char *newpath)
{
#if defined(__is_libk)
    (void)oldpath; (void)newpath;
    return -1;
#else
    int64_t ret = syscall(SYS_rename,
                          (uint64_t)oldpath, (uint64_t)newpath, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
