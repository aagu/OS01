#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int umask(int mode)
{
#if defined(__is_libk)
    (void)mode;
    return 0;
#else
    int64_t ret = syscall(SYS_umask, (uint64_t)mode, 0, 0);
    // umask always returns the old mask (0 for stub)
    return (int)ret;
#endif
}
