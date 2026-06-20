#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>

int64_t lseek(int fd, int64_t offset, int whence)
{
    return syscall(SYS_lseek, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence);
}
