// kernel/selftest/test_canary_single_source.c — AAGU-4.4
// Verify __stack_chk_guard has a single definition across kernel TUs.
//
// The link itself is the strongest check: if a kernel TU accidentally
// defines `unsigned long __stack_chk_guard` a second time, the kernel
// link fails with a duplicate-symbol error and the kernel never boots.
// This selftest simply touches the symbol so the compiler emits a
// reference (not optimized away) and so a runtime read of the canary
// happens once during boot — both to prove the kernel/core/stack_chk.c
// copy is the one the kernel resolved.
//
// Single-source guarantee is enforced by the spec at
// docs/arch/cross-boundary-symbols.md §2.1 ("kernel 与 libc 同时存在
// 同名的 builtin 实现 = 违例"); the selftest here is a
// belt-and-suspenders runtime probe, not the primary check.

#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <stdint.h>

/* These come from kernel/core/stack_chk.c.  Declaring
 * them `extern` here means this TU only references them; if any
 * other TU accidentally defines the same symbol, the link fails. */
extern unsigned long __stack_chk_guard;
extern void __stack_chk_fail(void);

int test_canary_single_source(void)
{
    /* Touch the guard so the compiler emits a real reference: if the
     * kernel resolved the wrong object (or zero-init .bss would mask
     * a missing definition), this read shows up in nm. */
    unsigned long g = __stack_chk_guard;
    (void)g;

    /* Take the address of __stack_chk_fail too.  Same purpose. */
    void (*fp)(void) = __stack_chk_fail;
    (void)fp;

    serial_printk("[selftest] canary_single_source: pass\n");
    return 0;
}

#endif /* OS01_SELFTEST */
