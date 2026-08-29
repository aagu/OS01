/* aarch64 phase 1: kernel C entry point.
 *
 * After head.S drops to EL1, builds dual-TTBR page tables, enables the
 * MMU, jumps to the high-half kernel image, clears the high-half .bss,
 * installs the VBAR table, parks the DTB slot, and finally calls here.
 *
 * For Task 1 we just print "OS01 aarch64 phase1 boot ok" on the PL011
 * and idle in `wfi`.  Interrupts stay masked (Phase 1 keeps IRQs off
 * until Task 3 enables them explicitly).
 *
 * No libc, no printk.c linkage: per controller ruling R2 we use the
 * local `kputs()` and a tiny memset().
 */

#include <stdint.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>          /* arch_local_irq_disable (idempotent) */

/* Tiny helpers — libc/libk are NOT linked (LIBS := -nostdlib for
 * aarch64).  Just enough for our local use. */
static void aarch64_memset(void *dst, int c, uint64_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    while (n--) {
        *p++ = (uint8_t)c;
    }
}

/* Forward from pl011.c — `void kputs(const char *)`. */
void kputs(const char *s);
void pl011_init(void);
void pl011_putc(char c);
void arch_install_exception_vectors(void);

/* Symbol from head.S: address of the vector table in high half. */
extern char exception_vectors[];
extern uint8_t _bss_start[], _bss_end[];

/* boot message per controller ruling R2 — keep it terse and unambiguous. */
static const char banner[] = "OS01 aarch64 phase1 boot ok\n";

void aarch64_main(uint64_t dtb_base)
{
    (void)dtb_base;          /* not used in Task 1; Task 3 will parse.  */

    /* Idempotent: a re-entry (early bring-up debugging) shouldn't
     * unmask IRQs by accident. */
    arch_local_irq_disable();

    /* Bring up PL011 (QEMU already works with reset values, but it
     * doesn't hurt to program LCR_H+CR for a clean state). */
    pl011_init();

    /* Hello, world. */
    kputs(banner);

    /* Done: idle forever.  PL011 stays the only output channel. */
    for (;;) {
        arch_cpu_halt();         /* wfi */
    }
}

/* Provide an explicit clear of the high-half `.bss` range at first call.
 * We don't need it at this point; head.S already cleared `.boot.bss` and
 * the trampoline in head.S clears high `.bss` before entry here.  Kept
 * for symmetry; never called in phase 1.  Marked __attribute__((used))
 * so the linker retains it (currently relies on attribute) — when Task 3
 * begins using global scratch state, this becomes the canonical path. */
__attribute__((used))
static void aarch64_clear_bss(void)
{
    aarch64_memset(_bss_start, 0, (uintptr_t)_bss_end - (uintptr_t)_bss_start);
}
