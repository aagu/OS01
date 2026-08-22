#include <unistd.h>
#include <sys/syscall.h>

pid_t getpgid(pid_t pid) {
    return (pid_t)syscall(SYS_getpgid, (uint64_t)pid, 0, 0);
}
