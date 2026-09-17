// libc/stdlib/getauxval.c — consume __libc_auxv (spec 2026-09-17 §6.3).
// Walk bounded to 64 pairs (same limit kernel emits); miss returns 0 with
// errno=ENOENT. Caller distinguishes miss from a real zero-valued entry by
// checking errno after the call.
#include <sys/auxv.h>
#include <errno.h>
#include <stdint.h>

extern uint64_t *__libc_auxv;      /* libc/csu/csu.c */

unsigned long getauxval(unsigned long type)
{
    if (!__libc_auxv) { errno = ENOENT; return 0; }
    for (int i = 0; i < 64; i++) {
        if (__libc_auxv[2 * i] == AT_NULL) break;
        if (__libc_auxv[2 * i] == type)
            return (unsigned long)__libc_auxv[2 * i + 1];
    }
    errno = ENOENT;
    return 0;
}
