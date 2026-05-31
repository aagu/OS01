#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int64_t read(const char *path, void *buffer, uint64_t size)
{
#if defined(__is_libk)
    // Not implemented for kernel mode (use vfs_read directly)
    return -1;
#else
    int64_t ret = syscall(SYS_read, (uint64_t)path, (uint64_t)buffer, size);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
#endif
}
