#include <unistd.h>
#include <sys/syscall.h>

pid_t getsid(pid_t pid) {
    return (pid_t)syscall(SYS_getsid, (uint64_t)pid, 0, 0);
}
