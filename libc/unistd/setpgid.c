#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

int setpgid(pid_t pid, pid_t pgid) {
    int64_t ret = syscall(SYS_setpgid, (uint64_t)pid, (uint64_t)pgid, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
