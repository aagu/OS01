#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>

time_t time(time_t *tloc)
{
#if defined(__is_libk)
    if (tloc) *tloc = 0;
    return 0;
#else
    return (time_t)syscall(SYS_time, (uint64_t)(uintptr_t)tloc, 0, 0);
#endif
}
