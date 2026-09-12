/* UEFI-only AArch64 BSP entry. APs enter secondary_idle independently. */
#include <stdint.h>
#include <core/bootinfo.h>
#include <log/log.h>      /* for log_err/log_info macros */
#include <memory/memory.h>   /* for struct boot_context / Virt_To_Phy */
#include <memory/pmm.h>      /* for PMMngr, struct Page, alloc_pages, free_pages, ZONE_NORMAL */
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/ram.h>
#include <arch/aarch64/smp.h>

void pl011_init(void);
void aarch64_extend_direct_map(void);
extern char exception_vectors[];

#if OS01_SELFTEST
/* Pre-SMP page-table round-trip against the active kernel root. The
 * helper validates the raw TTBR0_EL1 value, requires the self-test VA
 * to be initially absent, allocates one 4 KiB data page, maps it
 * kernel-RW + non-executable, exercises read/write through both the
 * self-test VA and the high-half direct-map alias, proves duplicate
 * map is EEXIST, then unmaps and confirms ENOENT. On any failure it
 * unmap/frees what it owns FIRST (per brief), then logs
 * `UEFI-A64: pt map smoke FAIL` and `[smp] FATAL: pt map selftest`,
 * and halts before hardware bring-up. No intermediate table
 * reclamation is attempted; only the unlinked data page and the
 * unlinked active leaf are torn down. */
static void aarch64_pt_smoke_test(void)
{
    /* State tracked for cleanup on failure: `fail_reason` records the
     * diagnostic suffix printed by the unified `fail` cleanup label.
     * `data_owned` tracks whether the allocated data page still needs
     * `free_4k_page`; `mapped` tracks whether the leaf is still live. */
    const char *fail_reason = NULL;
    uint64_t data_pa = 0;
    bool data_owned = false;
    bool mapped = false;
    uint64_t *root = NULL;

    /* Step 1: read raw TTBR0_EL1, reject bits outside
     * AARCH64_TTBR_BASE_MASK | AARCH64_TTBR_ALLOWED_NONBASE. */
    uint64_t ttbr_raw = (uint64_t)(uintptr_t)arch_get_page_table();
    if ((ttbr_raw & ~AARCH64_TTBR_ALLOWED_MASK) != 0) {
        fail_reason = "ttbr_raw has disallowed bits";
        goto fail;
    }

    /* Step 2: derive ttbr_pa. Nonzero, 4 KiB-aligned, < 1 TiB (IPS=40). */
    uint64_t ttbr_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    if (ttbr_pa == 0
        || (ttbr_pa & (PAGE_4K_SIZE - 1)) != 0
        || ttbr_pa >= (UINT64_C(1) << 40)) {
        fail_reason = "ttbr_pa invalid";
        goto fail;
    }

    /* Step 3: convert only the validated base to a direct-map pointer. */
    root = (uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET);

    /* Step 4: require AARCH64_PT_SELFTEST_VA to be initially absent. */
    uint64_t pa_q = 0;
    uint32_t perm_q = 0;
    int rc = aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA, &pa_q, &perm_q);
    if (rc != AARCH64_PT_ENOENT) {
        fail_reason = "initial query not ENOENT";
        goto fail;
    }

    /* Step 5: allocate one 4 KiB page and map it kernel-RW, non-exec. */
    data_pa = alloc_4k_page();
    if (data_pa == 0) {
        fail_reason = "alloc_4k_page returned 0";
        goto fail;
    }
    data_owned = true;
    int map_rc = aarch64_pt_map_4k(root, AARCH64_PT_SELFTEST_VA, data_pa,
                                   AARCH64_PT_KERNEL_RW);
    if (map_rc != AARCH64_PT_OK) {
        fail_reason = "map_4k failed";
        goto fail;
    }
    mapped = true;

    /* Step 6: write two distinct 64-bit sentinels through the VA and
     * verify they read back through BOTH the self-test VA and the
     * high-half direct-map alias of the same physical page. */
    volatile uint64_t *selftest_va =
        (volatile uint64_t *)(uintptr_t)AARCH64_PT_SELFTEST_VA;
    volatile uint64_t *direct =
        (volatile uint64_t *)(uintptr_t)(data_pa + ARCH_PAGE_OFFSET);
    const uint64_t SENTINEL_A = UINT64_C(0xa5a5a5a55a5a5a5a);
    const uint64_t SENTINEL_B = UINT64_C(0x5a5a5a5aa5a5a5a5);
    *selftest_va = SENTINEL_A;
    if (*selftest_va != SENTINEL_A || *direct != SENTINEL_A) {
        fail_reason = "sentinel A roundtrip failed";
        goto fail;
    }
    *selftest_va = SENTINEL_B;
    if (*selftest_va != SENTINEL_B || *direct != SENTINEL_B) {
        fail_reason = "sentinel B roundtrip failed";
        goto fail;
    }

    /* Step 7a: query the same PA and KERNEL_RW/non-exec permission. */
    uint64_t pa_q2 = 0;
    uint32_t perm_q2 = 0;
    int qrc = aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA,
                                  &pa_q2, &perm_q2);
    if (qrc != AARCH64_PT_OK
        || pa_q2 != data_pa
        || perm_q2 != AARCH64_PT_KERNEL_RW) {
        fail_reason = "post-map query mismatch";
        goto fail;
    }

    /* Step 7b: prove a second map returns EEXIST. */
    int rc2 = aarch64_pt_map_4k(root, AARCH64_PT_SELFTEST_VA, data_pa,
                                AARCH64_PT_KERNEL_RW);
    if (rc2 != AARCH64_PT_EEXIST) {
        fail_reason = "second map not EEXIST";
        goto fail;
    }

    /* Step 7c: unmap, expect the prior PA and permission. */
    uint64_t unmapped_pa = 0;
    uint32_t unmapped_perm = 0;
    int urc = aarch64_pt_unmap_4k(root, AARCH64_PT_SELFTEST_VA,
                                  &unmapped_pa, &unmapped_perm);
    if (urc != AARCH64_PT_OK
        || unmapped_pa != data_pa
        || unmapped_perm != AARCH64_PT_KERNEL_RW) {
        fail_reason = "unmap_4k failed";
        /* Best-effort retry: the brief asks us to free what's owned
         * when possible; the unmap may have failed for the same
         * reason the leaf is stuck, but a second attempt costs nothing
         * and matches the brief's "unmap/free any resource currently
         * owned" wording. */
        if (mapped) {
            (void)aarch64_pt_unmap_4k(root, AARCH64_PT_SELFTEST_VA, NULL, NULL);
        }
        goto fail;
    }
    mapped = false;

    /* Step 7d: a later query must return ENOENT. */
    int qrc2 = aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA,
                                   &pa_q2, &perm_q2);
    if (qrc2 != AARCH64_PT_ENOENT) {
        fail_reason = "post-unmap query not ENOENT";
        goto fail;
    }

    /* Step 8: free the data page and emit the success marker exactly. */
    free_4k_page(data_pa);
    log_info("UEFI-A64: pt map smoke OK\n");
    return;

fail:
    /* Per brief: unmap/free any resource currently owned FIRST, then
     * log the diagnostic markers, then halt. */
    if (mapped && root != NULL) {
        (void)aarch64_pt_unmap_4k(root, AARCH64_PT_SELFTEST_VA, NULL, NULL);
    }
    if (data_owned) {
        free_4k_page(data_pa);
    }
    log_err("UEFI-A64: pt map smoke FAIL\n");
    if (fail_reason != NULL) {
        log_err("[smp] FATAL: pt map selftest: %s\n", fail_reason);
    } else {
        log_err("[smp] FATAL: pt map selftest\n");
    }
    for (;;) arch_cpu_halt();
}
#endif

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
    /* head.S build_pagetables only writes PMD_low1[0]; slots 1..511
     * are left zero, so physical 0x40200000..0x80000000 has no
     * direct-map alias. Without this fixup the first runtime
     * alloc_4k_page() whose PA lies above 0x40200000 faults in
     * pmm.c when it writes the subpage_pool header. The fixup walks
     * the installed PGD → PUD → PMD_low1 and fills the missing slots
     * with 2 MiB Normal kernel-RW non-exec block descriptors. Pre-SMP
     * single-threaded; mutates the active root. */
    aarch64_extend_direct_map();
    aarch64_pt_smoke_test();
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
