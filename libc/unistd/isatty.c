#include <sys/syscall.h>
#include <stdint.h>

int isatty(int fd)
{
#if defined(__is_libk)
    (void)fd;
    return 0;
#else
    // Try TCGETS (0x5401) — if it succeeds, this is a terminal
    char dummy[64];
    return syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)0x5401, (uint64_t)dummy) == 0;
#endif
}
