#include <sys/types.h>   /* pid_t */
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>

int killpg(pid_t pgrp, int sig) {
    if (pgrp < 1) { errno = EINVAL; return -1; }
    // Same wrapping as libc kill() (kill.c): negative result → -1 + errno
    int64_t ret = syscall(SYS_kill,
                          (uint64_t)(-(int64_t)(int)pgrp),
                          (uint64_t)sig, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
