#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

pid_t getpgid(pid_t pid) {
    int64_t ret = syscall(SYS_getpgid, (uint64_t)pid, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}
