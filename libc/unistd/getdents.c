#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>

int getdents64(int fd, struct linux_dirent64 *buf, unsigned int count)
{
    return (int)syscall(SYS_getdents64, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}
