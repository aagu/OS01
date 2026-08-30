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
 *   - all other x0 values: direct boot's original FDT-or-zero argument.  A
 *     scratch boot_context retains the original FDT fallback semantics.
 *
 * Boot sequence:
 *   1. arch_local_irq_disable() (idempotent — DAIF already masked in head.S)
 *   2. pl011_init() — program PL011 for polled output
 *   3. Re-point VBAR_EL1 to the high-half exception_vectors
 *   4. dtb_init(boot_context.firmware.dtb) — minimal FDT parse
 *   5. gic_init() — distributor + CPU interface, only PPI 30 enabled
 *   6. arch_tick_start() — arm CNTP for one period
 *   7. direct boot only: smp_boot_aps() brings up APs and runs the SMP
 *      benchmark.  UEFI intentionally remains single-BSP, but retains
 *      the GIC, timer, and IRQ initialization in the surrounding steps.
 *   8. arch_local_irq_enable() — last step; tick ISR now runs
 *   9. wfi idle loop; the ISR prints "[tick] N" once a second
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

#define AARCH64_UEFI_HANDOFF_ADDRESS UINT64_C(0x401e0000)

enum aarch64_boot_mode {
    AARCH64_BOOT_DIRECT_ZERO,
    AARCH64_BOOT_DIRECT_FDT,
    AARCH64_BOOT_UEFI,
    AARCH64_BOOT_CORRUPT,
};

/* This function intentionally has no architectural instructions or boot
 * context construction so the host ABI test can compile the real selector. */
enum aarch64_boot_mode aarch64_select_boot_mode(
    uint64_t x0, const struct boot_context *fixed_context)
{
    if (x0 != AARCH64_UEFI_HANDOFF_ADDRESS)
        return x0 == 0 ? AARCH64_BOOT_DIRECT_ZERO : AARCH64_BOOT_DIRECT_FDT;
    return boot_context_valid(fixed_context) ?
        AARCH64_BOOT_UEFI : AARCH64_BOOT_CORRUPT;
}

#ifndef AARCH64_BOOT_MODE_HOST_TEST

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

/* Forward from smp.c — Task 4b brings up APs and runs the 4-core
 * benchmark in one call (see spec §2.1 v11 — the BSP writes
 * done[0]=1, acquires all done[i], and reports PASS/FAIL). */
void smp_boot_aps(void);

/* Symbol from head.S: address of the vector table in high half. */
extern char exception_vectors[];
extern uint8_t _bss_start[], _bss_end[];

/* boot message per controller ruling R2 — keep it terse and unambiguous. */
static const char banner[] = "OS01 aarch64 phase1 boot ok\n";
static const char uefi_handoff_banner[] = "OS01 aarch64 uefi handoff ok\n";
static const char corrupt_handoff_banner[] = "UEFI-A64: corrupt handoff\n";

void aarch64_main(const struct boot_context *handoff)
{
    struct boot_context scratch;
    const struct boot_context *bootctx;
    enum aarch64_boot_mode boot_mode;
    uint64_t boot_cpu_id;

    boot_mode = aarch64_select_boot_mode((uint64_t)(uintptr_t)handoff,
                                         handoff);
    if (boot_mode == AARCH64_BOOT_CORRUPT) {
        pl011_init();
        kputs(corrupt_handoff_banner);
        for (;;) {
            arch_cpu_halt();
        }
    }

    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(boot_cpu_id));
    if (boot_mode == AARCH64_BOOT_UEFI) {
        bootctx = handoff;
    } else {
        boot_context_from_aarch64(&scratch, (uint64_t)(uintptr_t)handoff,
                                  boot_cpu_id);
        bootctx = &scratch;
    }

    /* Step 1: idempotent — a re-entry (early bring-up debugging)
     * shouldn't unmask IRQs by accident. */
    arch_local_irq_disable();

    /* Step 2: PL011.  QEMU already works with reset values, but
     * it doesn't hurt to program LCR_H+CR for a clean state. */
    pl011_init();

    /* Step 3: re-point VBAR_EL1 to the high-half vector table.
     * See file header for the rationale.  The high-half
     * exception_vectors is 2 KiB-aligned by construction (see
     * entry.S) so VBAR accepts it directly. */
    {
        uint64_t vbar = (uint64_t)(uintptr_t)exception_vectors;
        __asm__ __volatile__("msr vbar_el1, %0" :: "r"(vbar) : "memory");
        __asm__ __volatile__("isb" ::: "memory");
    }

    /* Step 4: DTB.  Parses 5 nodes (/cpus, /psci, /timer,
     * /interrupt-controller, /pl011) and enforces the spec §2.1
     * failure conditions (CPU>NR_CPUS / duplicate MPIDR / BSP
     * MPIDR not in table).  If the DTB is missing or unparsable,
     * QEMU virt defaults are used. */
    dtb_init(bootctx->firmware.dtb);

    /* Hello, world.  Printed after dtb_init so a DTB parse failure
     * produces a [dtb] PANIC line before the banner. */
    if (boot_mode == AARCH64_BOOT_UEFI)
        kputs(uefi_handoff_banner);
    kputs(banner);

    /* Step 5: GICv2.  Distributor + CPU interface, only PPI 30. */
    gic_init();

    /* Step 6: CNTP physical-timer period mode.  Arms TVAL with
     * period = CNTFRQ_EL0 / HZ and enables the timer.  The first
     * tick will fire only after step 8 unmasks IRQs. */
    if (!arch_tick_start()) {
        kputs("[cntp] arch_tick_start FAILED\n");
        for (;;) {
            arch_cpu_halt();
        }
    }

    /* Direct boot runs Task 4b's SMP bring-up + 4-core benchmark after
     * the tick is configured but before IRQs are unmasked.  The UEFI
     * handoff deliberately remains single-BSP: its firmware started this
     * CPU, while GIC, timer, and IRQ setup below stay unchanged. */
    if (boot_mode != AARCH64_BOOT_UEFI)
        smp_boot_aps();

    /* Step 8: ENABLE IRQs.  Per spec §2.3 this is the last init
     * step.  After this, the tick ISR fires every 10 ms and
     * prints "[tick] N" once a second. */
    kputs("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    /* Context-sync: ensure the DAIF.I clear is visible to subsequent
     * exception entry before we enter the idle wait. */
    __asm__ __volatile__("isb" ::: "memory");

    /* Step 9: idle forever.  The tick ISR will return via eret
     * back to the wfi; the BSP never leaves this loop. */
    for (;;) {
        __asm__ __volatile__("dsb sy" ::: "memory");
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

#endif /* AARCH64_BOOT_MODE_HOST_TEST */
