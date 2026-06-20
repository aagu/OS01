#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>

int stat(const char *path, struct stat *buf)
{
    return (int)syscall(SYS_stat, (uint64_t)path, (uint64_t)buf, 0);
}
