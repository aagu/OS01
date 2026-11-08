// libc/unistd/lseek.c — lseek(2) wrapper (T9).
//
// The raw kernel answer is negative-errno encoded; POSIX wants -1 with
// errno set.  Without this mapping every seek failure leaked a negative
// return (e.g. -29 on a pipe) to callers that only check -1, and left
// errno stale.  ESPIPE (pipe) and EINVAL (bad whence) arrive unchanged
// from the kernel — the wrapper only re-encodes them.

#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>

int64_t lseek(int fd, int64_t offset, int whence)
{
    int64_t r = (int64_t)syscall(SYS_lseek, (uint64_t)fd,
                                 (uint64_t)offset, (uint64_t)whence);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}
