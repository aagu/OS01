#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>

int access(const char *path, int mode)
{
    return (int)syscall(SYS_access, (uint64_t)path, (uint64_t)mode, 0);
}
