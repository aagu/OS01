#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdarg.h>

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    uint64_t arg = va_arg(ap, uint64_t);
    va_end(ap);
    return (int)syscall(SYS_fcntl, (uint64_t)fd, (uint64_t)cmd, arg);
}
