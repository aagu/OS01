#ifndef _KERNEL_ASSERT_H
#define _KERNEL_ASSERT_H

#include <log/log.h>
#include <arch/cpu.h>

/* arch_cpu_halt (arch facade) instead of raw `hlt` inline asm: ASSERT is
 * compiled into arch-neutral TUs (fair.c reaches the aarch64 compile
 * check), and the spec (§8.3/§9) requires halts go through the arch
 * helper rather than leaving x86 instructions in common code. */
#define ASSERT(x) do { \
    if (!(x)) { \
        log_err("ASSERT failed: %s at %s:%d\n", #x, __FILE__, __LINE__); \
        while (1) { arch_cpu_halt(); } \
    } \
} while (0)

#endif /* _KERNEL_ASSERT_H */
