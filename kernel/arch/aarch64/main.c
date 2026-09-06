/* UEFI-only AArch64 BSP entry. APs enter secondary_idle independently. */
#include <stdint.h>
#include <kernel/bootinfo.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>
#include <kernel/arch/aarch64/boot_log.h>
#include <kernel/arch/aarch64/dtb.h>
#include <kernel/arch/aarch64/ram.h>
#include <kernel/arch/aarch64/smp.h>

void pl011_init(void);
extern char exception_vectors[];

void aarch64_main(const struct boot_context *handoff)
{
    arch_local_irq_disable();
    pl011_init();
    if (!boot_context_valid(handoff)) {
        log_err("UEFI-A64: corrupt handoff\n");
        log_err("[smp] FATAL: invalid UEFI handoff\n");
        for (;;) arch_cpu_halt();
    }
    uint64_t vbar = (uint64_t)(uintptr_t)exception_vectors;
    __asm__ __volatile__("msr vbar_el1, %0\n\tisb" :: "r"(vbar) : "memory");

    /* Turn the raw UEFI memory map into the published 2 MiB-aligned
     * aarch64_ram_map before any further hardware bring-up. The
     * helper halts the BSP on failure, so a non-zero return here
     * means the BSP is already gone. */
    aarch64_ram_init(handoff);

    /* Invalid or missing platform information is FATAL here, before any
     * GIC or PSCI access. Only valid platforms can degrade and keep ticks. */
    dtb_init(handoff);
    log_info("OS01 aarch64 uefi handoff ok\n");
    log_info("OS01 aarch64 phase1 boot ok\n");
    gic_init();

    uint32_t active = smp_boot_aps();
    if (active == dtb_cpu_count())
        (void)test_spinlock_smp(active);
    else
        log_warn("[spinlock] status=SKIP\n");

    /* BSP-only timer and IRQs begin after AP startup/testing has settled. */
    if (!arch_tick_start()) {
        log_err("[smp] FATAL: BSP timer initialization failed\n");
        for (;;) arch_cpu_halt();
    }
    log_info("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
    for (;;) arch_cpu_halt();
}
