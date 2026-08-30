/* aarch64 phase 1: kernel C entry point.
 *
 * After head.S drops to EL1, builds dual-TTBR page tables, enables the
 * MMU, jumps to the high-half kernel image, clears the high-half .bss,
 * installs the VBAR table, preserves x0 across the transition, and finally
 * calls here.
 *
 * Entry ABI:
 *   - x0 == 0x401e0000: AArch64 UEFI's fixed boot_context.  It must pass
 *     boot_context_valid(); malformed contexts are reported via PL011 and
 *     halt before normal initialization.
 *
 * Boot sequence:
 *   1. arch_local_irq_disable() (idempotent — DAIF already masked in head.S)
 *   2. pl011_init() — program PL011 for polled output
 *   3. Re-point VBAR_EL1 to the high-half exception_vectors
 *   4. dtb_init(boot_context.firmware.dtb) — minimal FDT parse
 *   5. gic_init() — distributor + CPU interface, only PPI 30 enabled
 *   6. arch_tick_start() — arm CNTP for one period
 *   7. arch_local_irq_enable() — last step; tick ISR now runs
 *   8. wfi idle loop; the ISR prints "[tick] N" once a second
 *
 * Per spec §2.3, `arch_local_irq_enable()` is the LAST init step.
 *
 * Note on VBAR: head.S installed the early `boot_vectors` (panic-
 * only) before MMU enable and never switched it, so by default the
 * EL1h IRQ slot would spin in boot_vectors.Lirq_spin instead of
 * dispatching to our C ISR.  Step 3 re-points VBAR_EL1 to the high-
 * half `exception_vectors`, which entry.S populates with the actual
 * `bl el1_irq_dispatch; eret` sequence.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/bootinfo.h>

#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>          /* arch_local_irq_disable / enable */

/* Tiny helpers — libc/libk are NOT linked (LIBS := -nostdlib for
 * aarch64).  Just enough for our local use. */
static void aarch64_memset(void *dst, int c, uint64_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    while (n--) {
        *p++ = (uint8_t)c;
    }
}

/* Forward from pl011.c. */
void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);
void pl011_init(void);
void pl011_putc(char c);
void arch_install_exception_vectors(void);

/* Forward from dtb.c. */
void dtb_init(uint64_t dtb_base);

/* Forward from gic.c. */
void gic_init(void);

/* Forward from time.c — declared in kernel/include/kernel/arch/cpu.h. */
bool arch_tick_start(void);

/* Symbol from head.S: address of the vector table in high half. */
extern char exception_vectors[];
extern uint8_t _bss_start[], _bss_end[];

/* boot message per controller ruling R2 — keep it terse and unambiguous. */
static const char banner[] = "OS01 aarch64 phase1 boot ok\n";
static const char uefi_handoff_banner[] = "OS01 aarch64 uefi handoff ok\n";
static const char corrupt_handoff_banner[] = "UEFI-A64: corrupt handoff\n";

void aarch64_main(const struct boot_context *handoff)
{
    if (!boot_context_valid(handoff)) {
        pl011_init();
        kputs(corrupt_handoff_banner);
        for (;;) {
            arch_cpu_halt();
        }
    }

    /* Step 1: idempotent — a re-entry (early bring-up debugging)
     * shouldn't unmask IRQs by accident. */
    arch_local_irq_disable();

    /* Step 2: PL011.  QEMU already works with reset values, but
     * it doesn't hurt to program LCR_H+CR for a clean state. */
    pl011_init();

    /* Step 3: re-point VBAR_EL1 to the high-half vector table. */
    {
        uint64_t vbar = (uint64_t)(uintptr_t)exception_vectors;
        __asm__ __volatile__("msr vbar_el1, %0" :: "r"(vbar) : "memory");
        __asm__ __volatile__("isb" ::: "memory");
    }

    /* Step 4: DTB.  Parses 5 nodes (/cpus, /psci, /timer,
     * /interrupt-controller, /pl011); missing/unparsable DTB falls back
     * to QEMU virt defaults. */
    dtb_init(handoff->firmware.dtb);

    kputs(uefi_handoff_banner);
    kputs(banner);

    /* Step 5: GICv2.  Distributor + CPU interface, only PPI 30. */
    gic_init();

    /* Step 6: CNTP physical-timer period mode. */
    if (!arch_tick_start()) {
        kputs("[cntp] arch_tick_start FAILED\n");
        for (;;) {
            arch_cpu_halt();
        }
    }

    /* Step 8: ENABLE IRQs.  After this the tick ISR fires every 10 ms
     * and prints "[tick] N" once a second. */
    kputs("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");

    for (;;) {
        __asm__ __volatile__("dsb sy" ::: "memory");
        arch_cpu_halt();         /* wfi */
    }
}

/* Provide an explicit clear of the high-half `.bss` range at first call.
 * We don't need it at this point; head.S already cleared `.boot.bss` and
 * the trampoline in head.S clears high `.bss` before entry here.  Kept
 * for symmetry; never called in phase 1.  Marked __attribute__((used))
 * so the linker retains it. */
__attribute__((used))
static void aarch64_clear_bss(void)
{
    aarch64_memset(_bss_start, 0, (uintptr_t)_bss_end - (uintptr_t)_bss_start);
}
