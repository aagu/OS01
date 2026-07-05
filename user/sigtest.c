/* sigtest — standalone signal handler test.  Run directly from shell:
 *   /sigtest.elf
 * Registers SIGUSR1 handler, sends to self, exits 0 on success. */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

static volatile int got = 0;
static void handler(int sig) { (void)sig; got = 1; }

int main(void)
{
    printf("sigtest: pid=%d testing signal handler...\n", (int)getpid());

    signal(SIGUSR1, handler);
    printf("sigtest: handler registered at %p\n", (void*)handler);

    kill(getpid(), SIGUSR1);
    printf("sigtest: kill returned, got=%d\n", got);

    if (!got) {
        printf("FAIL: handler not called\n");
        return 1;
    }

    printf("PASS: signal handler sync\n");
    return 0;
}
