#include <sys/syscall.h>

/* _exit — immediate syscall termination, no atexit handlers.
 * Goes through the generic syscall() helper so the inline asm stays
 * per-arch in libc/include/sys/syscall.h (x86: int $0x80, aarch64:
 * svc #0).  The syscall never returns; the loop below only satisfies
 * noreturn analysis on a spurious return. */
void _exit(int status)
{
    (void)syscall(SYS_exit, (uint64_t)status, 0, 0);
    for (;;)
        ;
}
