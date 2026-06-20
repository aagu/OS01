#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int mknod(const char *path, mode_t mode, dev_t dev)
{
    (void)path; (void)mode; (void)dev;
    errno = ENOSYS;
    return -1;
}
