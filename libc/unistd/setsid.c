#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

// POSIX: returns the new session ID (== caller's pid) on success, or -1 + errno
pid_t setsid(void) {
    int64_t ret = syscall(SYS_setsid, 0, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}
