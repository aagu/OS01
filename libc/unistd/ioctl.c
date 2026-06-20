#include <sys/syscall.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdarg.h>

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    uint64_t arg = va_arg(ap, uint64_t);
    va_end(ap);
    return (int)syscall(SYS_ioctl, (uint64_t)fd, request, arg);
}
