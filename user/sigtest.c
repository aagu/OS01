// sigtest — signal delivery verification
//
// Blocks on stdin.  If SIGINT (Ctrl-C) is delivered correctly,
// SIG_DFL -> do_exit and we never reach the printf at the end.
// If we do reach it, signal delivery is broken.

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    int pid = (int)syscall(SYS_getpid, 0, 0, 0);
    printf("sigtest: pid=%d blocking on stdin (Ctrl-C to kill)\n", pid);

    char buf[256];
    int n = (int)syscall(SYS_read, 0, (uint64_t)buf, sizeof(buf));

    // We should never reach here if Ctrl-C delivered SIGINT ->
    // SIG_DFL -> do_exit.  If we do, signal delivery is broken.
    printf("sigtest: read returned %d (pid=%d) -- SIGINT NOT delivered!\n", n, pid);
    return 1;
}
