#include <sys/time.h>
#include <sys/syscall.h>
#include <errno.h>

// Stub: utimes not implemented
int utimes(const char *filename, const struct timeval times[2])
{
    (void)filename; (void)times;
    errno = ENOSYS;
    return -1;
}
