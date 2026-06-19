#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int chdir(const char *path)
{
#if defined(__is_libk)
    (void)path;
    return -1;
#else
    int64_t ret = syscall(SYS_chdir, (uint64_t)path, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
