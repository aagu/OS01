#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>

int fstat(int fd, struct stat *buf)
{
    return (int)syscall(SYS_fstat, (uint64_t)fd, (uint64_t)buf, 0);
}
