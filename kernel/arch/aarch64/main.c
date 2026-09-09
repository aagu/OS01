/* UEFI-only AArch64 BSP entry. APs enter secondary_idle independently. */
#include <stdint.h>
#include <kernel/bootinfo.h>
#include <kernel/log.h>      /* for log_err/log_info macros */
#include <kernel/memory.h>   /* for struct boot_context / Virt_To_Phy */
#include <kernel/pmm.h>      /* for PMMngr, struct Page, alloc_pages, free_pages, ZONE_NORMAL */
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>
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

    /* Populate PMMngr fields that pmm_init reads. Mirrors the
     * kernel/kernel/main.c:155-159 prelude on x86_64, but uses the
     * aarch64 VMA linker symbols (_text_start/_text_end/.../_kernel_end)
     * because _text/_edata/_end do not exist on aarch64. */
    extern char _text_start[], _text_end[];
    extern char _rodata_end[];
    extern char _data_end[];
    extern char _kernel_end[];

    /* Sanity check: the aarch64 identity map must be active before
     * pmm_init runs (otherwise Virt_To_Phy on high-half VMAs returns
     * nonsense and the kernel-image walk in Step 7 silently corrupts
     * pages_struct[]). head.S installs the identity map before
     * dropping to C. */
    if ((uint64_t)&_text_start < ARCH_PAGE_OFFSET) {
        log_err("[smp] FATAL: aarch64 identity map not active\n");
        arch_cpu_halt();
    }

    PMMngr.start_code  = (uint64_t)&_text_start;
    PMMngr.end_code    = (uint64_t)&_text_end;
    PMMngr.end_data    = (uint64_t)&_data_end;
    PMMngr.end_rodata  = (uint64_t)&_rodata_end;
    PMMngr.start_brk   = (uint64_t)&_kernel_end;

    pmm_init(handoff);

#if OS01_SELFTEST
    {
        struct Page *p = alloc_pages(ZONE_NORMAL, 1, 0);
        if (p) { free_pages(p, 1); log_info("UEFI-A64: pmm alloc smoke OK\n"); }
        else   { log_err("UEFI-A64: pmm alloc smoke FAIL\n"); }
    }
#endif

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
