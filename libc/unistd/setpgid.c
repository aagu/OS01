#include <unistd.h>
#include <sys/syscall.h>

int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall(SYS_setpgid, (uint64_t)pid, (uint64_t)pgid, 0);
}
