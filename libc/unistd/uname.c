#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

int uname(struct utsname *buf)
{
#if defined(__is_libk)
    (void)buf;
    return -1;
#else
    int64_t ret = syscall(SYS_uname, (uint64_t)buf, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
#endif
}
