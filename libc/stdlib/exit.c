// libc/stdlib/exit.c — POSIX exit(3)
//
// Runs registered atexit handlers in LIFO order (POSIX-mandated), then
// terminates the process via the SYS_exit syscall. The atexit handlers
// live in libc/stdlib/atexit.c (`__call_atexit_handlers`); this file
// just glues them onto the process exit path.
//
// Wiring: csu.c's __libc_start_main calls exit(main(...)) instead of
// returning main's status directly, so main's natural `return n;` and
// an explicit `exit(n)` both reach this routine.
//
// Note: there is also a `static inline void exit(int)` in
// <sys/syscall.h> (raw SYS_exit wrapper). <stdlib.h> declares the real
// exit() and is included by callers before <sys/syscall.h>, so the
// out-of-line definition here wins and the inline wrapper is shadowed.
#include <stdlib.h>
#include <unistd.h>

extern void __call_atexit_handlers(void);

__attribute__((__noreturn__))
void exit(int status)
{
    __call_atexit_handlers();
    _exit(status);
    __builtin_unreachable();
}
