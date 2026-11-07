/* UEFI-only AArch64 BSP entry. APs enter secondary_idle independently. */
#include <stdint.h>
#include <string.h>
#include <arch/boot.h>
#include <arch/boot_memory.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/page_table_selftest.h>
#include <core/bootinfo.h>
#include <log/log.h>      /* for log_err/log_info macros */
#include <core/printk.h>  /* for serial_printk (selftest markers) */
#include <arch/fb_write.h>            /* ARCH_NAME (via <arch/identity.h>) */
#include <driver/logo.h>              /* boot_logo_show */
#include <core/selftest.h> /* for selftest_run_all (OS01_SELFTEST builds) */
#include <selftest/result.h> /* protocol-v1 coordinator (OS01_SELFTEST builds) */
#include <memory/memory.h>   /* for struct boot_context / Virt_To_Phy */
#include <memory/pmm.h>      /* for PMMngr, struct Page, alloc_pages, free_pages, ZONE_NORMAL */
#include <memory/pmm_arch.h> /* for pmm_arch_normalize (preflight caller) */
#include <memory/vmm.h>      /* for arch_vmm_init (M3.5 Task 24 production call) */
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/boot/shootdown_probe.h>    /* aarch64_shootdown_probe (M3.5 Task 25) */
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/ram.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/ap_sched.h>
#include <arch/aarch64/vmm_gate.h> /* smp_starting_enter / ipi_ready_publish_and_count (M3 Task 11) */
#include <percpu/percpu.h>        /* percpu_data / num_cpus (M3 Task 11) */
#include <sched/task.h>           /* init_task_union / STACK_SIZE */
#include <subsys/subsys.h>   /* SUBSYS_PHASE_4 macro + subsys_init_phase() decl
                              * for SUBSYS_INITCALL Task 2 R3-1 register+dispatch
                              * pair. Lightweight header — only <stdint.h>
                              * transitively; does NOT pull in <arch/subsys.h>. */

void pl011_init(void);
extern char exception_vectors[];

/* Forward declarations for boot_log helpers + GIC dispatch probes
 * (Task 2.2).  Defined in kernel/arch/aarch64/boot_log.h / irq_probe.c
 * (the latter is OS01_SELFTEST-gated). */
void kputs(const char *s);
void kputu(uint64_t v);
/* Forward declarations for the arch-neutral clocksource framework
 * (aarch64 Generic Timer Task 2.2 GREEN). We intentionally do NOT
 * `#include <time/clocksource.h>` here because its transitive
 * `<time/timer.h>` pulls in `<list.h>`, which the aarch64 kernel
 * profile does not expose (no libc sysroot). The forward decls are
 * enough for our 3-line marker-print below. */
extern bool     clocksource_active;
extern uint64_t clocksource_freq_hz(void);
extern uint32_t clocksource_mult;
extern uint32_t clocksource_shift;
#if OS01_SELFTEST
void gic_clobber_probe(void);
void gic_unexpected_probe(void);
void gic_spi_test_init(void);
void gic_ipi_test(uint32_t cpu_count);
#endif

#if OS01_SELFTEST
/* ── Boot-map selftest (M0) ────────────────────────────────────────
 * Validates the descriptors head.S installed in the active TTBR0
 * root. head.S fills all 512 PMD_low1 slots before the MMU is
 * enabled; this walk proves the installed descriptors match the
 * spec contract (complete descriptor encoding) for PMD_low1[1..511].
 *
 * The walk reads the ACTUAL installed entries — not a constructor's
 * return value: PGD[0] → PUD[0] → PMD_low0 and PUD[1] → PMD_low1,
 * each link checked for table-descriptor type (bits[1:0]=11) and a
 * sane physical base before being followed through ARCH_PAGE_OFFSET.
 *
 * Special low-map leaves use field-wise checks for precise mismatch
 * reasons; PMD_low1[1..511] use exact descriptor equality so no
 * unexpected or reserved bit can pass unnoticed.
 * Expected full descriptors after the head.S fix:
 *   PMD_low0[0]    = 0x60000000000705  (PA=0,          Normal 0x705, PXN|UXN)
 *   PMD_low0[0x40] = 0x60000008000401  (PA=0x08000000, Device 0x401, PXN|UXN)
 *   PMD_low1[0]    = 0x40000040000705  (PA=0x40000000, Normal 0x705, UXN only)
 *   PMD_low1[i]    = 0x60000000000705 | (0x40000000 + i*0x200000)
 *                    for i=1..511        (Normal 0x705, PXN|UXN)
 * ──────────────────────────────────────────────────────────────── */

/* Descriptor field masks. */
#define BOOT_MAP_TABLE_TYPE     UINT64_C(0x3)               /* bits[1:0]=11: table link  */
#define BOOT_MAP_BLOCK_TYPE     UINT64_C(0x1)               /* bits[1:0]=01: 2 MiB block */
#define BOOT_MAP_TABLE_PA_MASK  UINT64_C(0x000000FFFFFFF000) /* 40-bit PA bits [39:12] */
#define BOOT_MAP_BLOCK_PA_MASK  UINT64_C(0x000000FFFFE00000) /* 40-bit PA bits [39:21] */
#define BOOT_MAP_LOW_MASK       UINT64_C(0x7FF)              /* low flags bits [10:0];
 * AttrIndx is encoded at bits [4:2] and is included in this check. */
#define BOOT_MAP_PXN_BIT        UINT64_C(0x20000000000000)    /* bit 53                  */
#define BOOT_MAP_UXN_BIT        UINT64_C(0x40000000000000)    /* bit 54                  */
#define BOOT_MAP_CONTIG_BIT     UINT64_C(0x10000000000000)    /* bit 52: Contiguous hint */

/* Format a uint64 as decimal into buf. The aarch64 libk subset has no
 * snprintf, so the PMD_low1 walk uses this to build a per-slot `what`
 * prefix ("PMD_low1[123]") in a caller-owned buffer — boot_map_check_block
 * writes its reason into its OWN static buffer, so the prefix must not
 * live there. */
static void boot_map_fmt_u64(char *buf, uint64_t v)
{
    char tmp[24];
    int n = 0;
    int i = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (v != 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        buf[i++] = tmp[--n];
    }
    buf[i] = '\0';
}

/* Validate one intermediate table descriptor (a PGD/PUD link).
 * Requires bits[1:0]=11 (table) and a nonzero, 4 KiB-aligned
 * next-table PA below 1 TiB. On success stores the PA in *next_pa
 * and returns NULL; on failure returns a static-buffer reason
 * string prefixed with `what`. BSP pre-SMP single-threaded, so the
 * static buffer is safe. */
static const char *boot_map_check_table(const char *what, uint64_t desc,
                                        uint64_t *next_pa)
{
    static char reason[96];

    if ((desc & BOOT_MAP_TABLE_TYPE) != BOOT_MAP_TABLE_TYPE) {
        strcpy(reason, what);
        strcat(reason, ": not a table descriptor (bits[1:0] != 11)");
        return reason;
    }
    uint64_t pa = desc & BOOT_MAP_TABLE_PA_MASK;
    if (pa == 0
        || (pa & (PAGE_4K_SIZE - 1)) != 0
        || pa >= (UINT64_C(1) << 40)) {
        strcpy(reason, what);
        strcat(reason, ": next-table PA invalid");
        return reason;
    }
    *next_pa = pa;
    return NULL;
}

/* Validate one installed 2 MiB block descriptor (a PMD leaf).
 * Requires bits[1:0]=01 (block), PA field (bits [39:21]) equal to
 * expected_pa, low flags (bits [10:0]) equal to expected_low, bit 52
 * clear, and the PXN/UXN bits matching expected_exec. On success
 * returns NULL; on failure returns a static-buffer reason string
 * prefixed with `what`. expected_pa is a MASKED PA field (bits
 * [39:21]), not an address — mask before passing. */
static const char *boot_map_check_block(const char *what, uint64_t desc,
                                        uint64_t expected_pa,
                                        uint64_t expected_low,
                                        uint64_t expected_exec)
{
    static char reason[96];

    if ((desc & UINT64_C(0x3)) != BOOT_MAP_BLOCK_TYPE) {
        strcpy(reason, what);
        strcat(reason, ": not a 2 MiB block descriptor (bits[1:0] != 01)");
        return reason;
    }
    if ((desc & BOOT_MAP_BLOCK_PA_MASK) != expected_pa) {
        strcpy(reason, what);
        strcat(reason, ": PA field mismatch");
        return reason;
    }
    if ((desc & BOOT_MAP_LOW_MASK) != expected_low) {
        strcpy(reason, what);
        strcat(reason, ": low flags mismatch");
        return reason;
    }
    if ((desc & BOOT_MAP_CONTIG_BIT) != 0) {
        strcpy(reason, what);
        strcat(reason, ": bit 52 (Contiguous hint) set");
        return reason;
    }
    if ((desc & BOOT_MAP_PXN_BIT) != (expected_exec & BOOT_MAP_PXN_BIT)) {
        strcpy(reason, what);
        strcat(reason, ": PXN mismatch");
        return reason;
    }
    if ((desc & BOOT_MAP_UXN_BIT) != (expected_exec & BOOT_MAP_UXN_BIT)) {
        strcpy(reason, what);
        strcat(reason, ": UXN mismatch");
        return reason;
    }
    return NULL;
}

/* Validate a block descriptor whose complete encoding is part of the
 * bootstrap contract, including reserved or otherwise unexpected bits. */
static const char *boot_map_check_exact_block(const char *what, uint64_t desc,
                                              uint64_t expected_desc)
{
    static char reason[96];

    if (desc != expected_desc) {
        strcpy(reason, what);
        strcat(reason, ": descriptor mismatch (unexpected bits or fields)");
        return reason;
    }
    return NULL;
}

/* Scan entries beginning at first_slot, using the same descriptor
 * validation path for both the live table and the synthetic regression
 * descriptor. */
static const char *boot_map_check_pmd_low1_range(const uint64_t *entries,
                                                 uint64_t first_slot,
                                                 uint64_t count)
{
    for (uint64_t offset = 0; offset < count; ++offset) {
        uint64_t i = first_slot + offset;
        char what[32];
        strcpy(what, "PMD_low1[");
        boot_map_fmt_u64(what + strlen(what), i);
        strcat(what, "]");

        uint64_t expected_desc = UINT64_C(0x60000000000705)
                               + UINT64_C(0x40000000)
                               + i * UINT64_C(0x200000);
        const char *r = boot_map_check_exact_block(what, entries[offset],
                                                   expected_desc);
        if (r != NULL)
            return r;
    }
    return NULL;
}

/* Pre-SMP validation of the installed boot page tables. Runs after
 * the PMM alloc/free smoke and before the 4 KiB page-table smoke.
 * On any malformed table link or descriptor mismatch it logs the
 * FATAL marker and halts before GIC/SMP bring-up. */
static void aarch64_boot_map_selftest(void)
{
    const char *fail_reason = NULL;
    const char *r = NULL;
    uint64_t ttbr_raw = 0;
    uint64_t ttbr_pa = 0;
    uint64_t pud_pa = 0;
    uint64_t pmd_pa = 0;
    uint64_t *root = NULL;
    uint64_t *pud = NULL;
    const uint64_t *pmd_low0 = NULL;
    const uint64_t *pmd_low1 = NULL;

    /* Step 1: read raw TTBR0_EL1 and validate it exactly the way
     * aarch64_pt_smoke_test() does. */
    ttbr_raw = (uint64_t)(uintptr_t)arch_get_page_table();
    if ((ttbr_raw & ~AARCH64_TTBR_ALLOWED_MASK) != 0) {
        fail_reason = "ttbr_raw has disallowed bits";
        goto fail;
    }
    ttbr_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    if (ttbr_pa == 0
        || (ttbr_pa & (PAGE_4K_SIZE - 1)) != 0
        || ttbr_pa >= (UINT64_C(1) << 40)) {
        fail_reason = "ttbr_pa invalid";
        goto fail;
    }
    root = (uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET);

    /* Step 2: follow the ACTUAL installed table descriptors.
     * PGD[0] is the shared TTBR0/TTBR1 root → PUD_low. */
    r = boot_map_check_table("PGD[0]", root[0], &pud_pa);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }
    pud = (uint64_t *)(uintptr_t)(pud_pa + ARCH_PAGE_OFFSET);

    /* PUD[0] → PMD_low0 (physical 0..1 GiB). */
    r = boot_map_check_table("PUD[0]", pud[0], &pmd_pa);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }
    pmd_low0 = (const uint64_t *)(uintptr_t)(pmd_pa + ARCH_PAGE_OFFSET);

    /* PUD[1] → PMD_low1 (physical 1..2 GiB). */
    r = boot_map_check_table("PUD[1]", pud[1], &pmd_pa);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }
    pmd_low1 = (const uint64_t *)(uintptr_t)(pmd_pa + ARCH_PAGE_OFFSET);

    /* Step 3: PMD_low0 — one Normal block (slot 0) and one MMIO
     * Device block (slot 0x40). Kept separate from the PMD_low1
     * walk: these cover the low identity region, not the kernel
     * image direct map. Both must carry PXN|UXN. */
    r = boot_map_check_block("PMD_low0[0]", pmd_low0[0],
                             0, 0x705,
                             BOOT_MAP_PXN_BIT | BOOT_MAP_UXN_BIT);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }
    r = boot_map_check_block("PMD_low0[0x40]", pmd_low0[0x40],
                             UINT64_C(0x08000000), 0x401,
                             BOOT_MAP_PXN_BIT | BOOT_MAP_UXN_BIT);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }

    /* Step 4: PMD_low1[0] — the kernel-image block. Executable at
     * EL1 (PXN=0) but not at EL0 (UXN=1). */
    r = boot_map_check_block("PMD_low1[0]", pmd_low1[0],
                             UINT64_C(0x40000000), 0x705,
                             BOOT_MAP_UXN_BIT);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }

    /* Check once that the exact-descriptor helper rejects a PA bit that
     * lies above this 40-bit physical-address configuration. */
    uint64_t probe_expected_desc = UINT64_C(0x60000000000705)
                                 + UINT64_C(0x40000000)
                                 + UINT64_C(0x200000);
    const uint64_t probe_entry = probe_expected_desc | (UINT64_C(1) << 40);
    r = boot_map_check_pmd_low1_range(&probe_entry, 1, 1);
    if (r == NULL) {
        fail_reason = "PA bit 40 mutation was accepted";
        goto fail;
    }

    /* Step 5: PMD_low1[1..511] — the fixed bootstrap direct-map window
     * head.S fills before the MMU is enabled. Each slot must be a 2 MiB
     * Normal block at PA 0x40000000 + i*0x200000 with low flags 0x705,
     * PXN|UXN set and bit 52 clear. This checks the complete descriptor,
     * including reserved bits, in one assertion per slot. The
     * `what` prefix is formatted into a local buffer because
     * boot_map_check_exact_block writes its reason into its own static
     * buffer. */
    r = boot_map_check_pmd_low1_range(pmd_low1 + 1, 1, 511);
    if (r != NULL) {
        fail_reason = r;
        goto fail;
    }

    log_info("UEFI-A64: boot map selftest OK\n");
    return;

fail:
    log_err("[smp] FATAL: aarch64 boot map selftest: %s\n", fail_reason);
    for (;;) arch_cpu_halt();
}
#endif

/* Populate Pos from the UEFI-handoff graphics info and map the
 * framebuffer MMIO. Gate on BOOT_CONTEXT_HAS_FRAMEBUFFER so a system
 * without ramfb (or with broken GOP) still boots cleanly via PL011.
 *
 * Called once from the kernel_main boot sequence, after arch_vmm_init returns success. */
static void aarch64_boot_fb_init(const struct boot_context *handoff)
{
    /* spin_init(&Pos.lock) MUST run on every boot path — including the
     * no-framebuffer branch — because color_printk callers (slab.c error
     * paths) will spin_lock(&Pos.lock) unconditionally.  BSS zero-init
     * leaves .lock == 0, which means LOCKED per spinlock_T's contract
     * (1=unlocked).  spin_init before the gate keeps the lock usable on
     * every code path. */
    spin_init(&Pos.lock);

    if (!(handoff->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER)) {
        log_info("[fb] no framebuffer in handoff; color_printk disabled\n");
    } else {
        Pos.Phy_addr    = (uint32_t *)handoff->graphics.FrameBufferBase;
        Pos.FB_length   = handoff->graphics.FrameBufferSize;
        Pos.XResolution = handoff->graphics.HorizontalResolution;
        Pos.YResolution = handoff->graphics.VerticalResolution;
        Pos.XPosition   = 0;
        Pos.YPosition   = 0;

        frame_buffer_init();
        if (!Pos.FB_addr)
            log_warn("[fb] mapping failed; color_printk disabled\n");
    }

    /* OS01 boot logo.  Drawn FIRST so the [fb] banner below lands under
     * it (the logo advances Pos.YPosition past the wordmark).  The driver
     * is silent; the boot path reports the outcome. */
    if (boot_logo_show() == 0)
        log_info("[logo] boot logo drawn (%s, %ux%u)\n", ARCH_NAME,
                 (unsigned)Pos.XResolution, (unsigned)Pos.YResolution);
    else
        log_info("[logo] skipped: no framebuffer\n");

    if (Pos.FB_addr) {
        /* Screen test pattern: render test banner expected by
         * qemutests/aarch64_boot_logo.py (check: fb-visible). */
        color_printk(WHITE, BLUE,
                     "[fb] aarch64 color_printk active (%ux%u)\n",
                     Pos.XResolution, Pos.YResolution);
        log_info("[fb] frame buffer remap succeed (banner on screen)\n");
    }
}

void arch_early_panic(const char *msg)
{
    (void)msg;
    pl011_init();
    log_err("UEFI-A64: corrupt handoff\n");
    log_err("[smp] FATAL: invalid UEFI handoff\n");
    for (;;) arch_cpu_halt();
}

void arch_early_init(const struct boot_context *bootctx)
{
    arch_local_irq_disable();
    pl011_init();
    uint64_t vbar = (uint64_t)(uintptr_t)exception_vectors;
    __asm__ __volatile__("msr vbar_el1, %0\n\tisb" :: "r"(vbar) : "memory");

    /* Turn the raw UEFI memory map into the published 2 MiB-aligned
     * aarch64_ram_map before any further hardware bring-up. The
     * helper halts the BSP on failure, so a non-zero return here
     * means the BSP is already gone. */
    aarch64_ram_init(bootctx);
}

__attribute__((noinline))
static void aarch64_early_arena_preflight(const struct boot_context *handoff)
{
    struct MEMORY_RANGE scratch[MEMORY_RANGE_MAX];
    size_t n = pmm_arch_normalize(handoff, scratch);
    if (n == 0) {
        log_err("[smp] FATAL: pmm_arch_normalize returned no ranges\n");
        for (;;) arch_cpu_halt();
    }
    if (aarch64_early_arena_prepare(scratch, n) != 0) {
        log_err("[smp] FATAL: aarch64 M1 arena preflight failed\n");
        for (;;) arch_cpu_halt();
    }
}

void arch_boot_memory(const struct boot_context *bootctx)
{
    extern char _text_start[], _text_end[];
    extern char _rodata_end[];
    extern char _data_end[];
    extern char _kernel_end[];

    if ((uint64_t)&_text_start < ARCH_PAGE_OFFSET) {
        log_err("[smp] FATAL: aarch64 identity map not active\n");
        arch_cpu_halt();
    }

    PMMngr.start_code  = (uint64_t)&_text_start;
    PMMngr.end_code    = (uint64_t)&_text_end;
    PMMngr.end_data    = (uint64_t)&_data_end;
    PMMngr.end_rodata  = (uint64_t)&_rodata_end;
    PMMngr.start_brk   = (uint64_t)&_kernel_end;

    aarch64_early_arena_preflight(bootctx);

    pmm_init(bootctx);

#if OS01_SELFTEST
    {
        struct Page *p = alloc_pages(ZONE_NORMAL, 1, 0);
        if (p) { free_pages(p, 1); log_info("UEFI-A64: pmm alloc smoke OK\n"); }
        else   { log_err("UEFI-A64: pmm alloc smoke FAIL\n"); }
    }
    aarch64_boot_map_selftest();
#endif

    if (arch_boot_direct_map_init()) {
        log_err("M1 FATAL reason=runtime-init\n");
        for (;;) arch_cpu_halt();
    }

    {
        int rc = arch_vmm_init();
        if (rc) {
            kputs("FATAL: arch_vmm_init rc=-");
            kputu((uint64_t)(-(int64_t)rc));
            kputs("\n");
            for (;;) arch_cpu_halt();
        }
    }

    aarch64_boot_fb_init(bootctx);
    if (aarch64_page_table_probe_prepare()) {
        log_err("M1 FATAL reason=probe\n");
        for (;;) arch_cpu_halt();
    }
}

void arch_pre_subsys_selftest(const struct boot_context *bootctx)
{
    (void)bootctx;
#if defined(OS01_SELFTEST)
    {
        selftest_begin_run(arch_selftest_expected_late());
        (void)selftest_run_all();
    }
#endif

#if AARCH64_SYNC_FAULT_TEST
    {
        uint64_t ttbr_raw = aarch64_read_ttbr1();
        if ((ttbr_raw & ~AARCH64_TTBR_ALLOWED_MASK) != 0) {
            kputs("[aarch64-sync-test] precondition FAIL: ttbr_raw has disallowed bits\n");
            for (;;) arch_cpu_halt();
        }
        uint64_t ttbr_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
        if (ttbr_pa == 0
            || (ttbr_pa & (PAGE_4K_SIZE - 1)) != 0
            || ttbr_pa >= (UINT64_C(1) << 40)) {
            kputs("[aarch64-sync-test] precondition FAIL: ttbr_pa invalid\n");
            for (;;) arch_cpu_halt();
        }
        uint64_t *root = (uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET);
        uint64_t pa_q = 0;
        uint32_t perm_q = 0;
        int rc = aarch64_pt_query_4k(root, AARCH64_PT_SELFTEST_VA,
                                     &pa_q, &perm_q);
        if (rc != AARCH64_PT_ENOENT) {
            kputs("[aarch64-sync-test] precondition FAIL: VA not absent\n");
            for (;;) arch_cpu_halt();
        }
        kputs("[aarch64-sync-test] armed\n");
        uint64_t probed;
        __asm__ __volatile__(
            "ldr %0, [%2]\n\t"
            : "=r"(probed)
            : "m"(*(volatile uint64_t *)(uintptr_t)AARCH64_PT_SELFTEST_VA),
              "r"((uint64_t)(uintptr_t)AARCH64_PT_SELFTEST_VA)
            : "memory");
        kputs("[aarch64-sync-test] returned\n");
        (void)probed;
        for (;;) arch_cpu_halt();
    }
#endif
}

void arch_boot_subsystems(const struct boot_context *bootctx)
{
    dtb_init(bootctx);
    log_info("OS01 aarch64 uefi handoff ok\n");
    log_info("OS01 aarch64 phase1 boot ok\n");
    gic_init();
}

void arch_boot_percpu(void)
{
    extern void percpu_install_gs(uint32_t cpu);
    extern void percpu_init(uint32_t cpu, uint32_t apic_id);
    uint32_t mpidr_bsp;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_bsp));
    percpu_install_gs(0);
    /* BOOT-1: percpu_init only PREPARES data (online stays 0); this
     * release store is the BSP's explicit runtime online publication —
     * the single BSP publication point (spec §5.1).  APs publish their
     * own online in secondary_idle before their boot ACK. */
    percpu_init(0, mpidr_bsp);
    __atomic_store_n(&percpu_data[0].online, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE);
    smp_starting_enter();
}

#if AARCH64_SMP_TEST_NO_ACK_CPU == 1
/* BOOT-1 Task 4: the no-ack runtime-qualification observation.  The
 * NO_ACK AP never ACKs its boot slot (so it is never admitted), but it
 * DID publish its own runtime online and IPI readiness — exactly the
 * distinction the frozen admission contract rests on.  The BSP reads
 * CPU1's runtime/admission state AFTER the shootdown probe returned
 * (the probe is the construction-before-runtime-reader barrier) and
 * BEFORE the scheduler handoff; every value is acquired or read
 * through the existing accepted-mask API and asserted before the
 * record is emitted.  A mismatched precondition emits nothing, so the
 * harness rejects the run on the missing record instead of trusting a
 * fabricated one. */
static void boot1_no_ack_observation(void)
{
    if (dtb_cpu_count() < 2)
        return;
    uint32_t online = __atomic_load_n(&percpu_data[1].online,
                                      __ATOMIC_ACQUIRE);
    uint32_t ipi_ready = __atomic_load_n(&percpu_data[1].ipi_ready,
                                         __ATOMIC_ACQUIRE);
    uint32_t sched_ok = __atomic_load_n(&percpu_data[1].scheduler_ok,
                                        __ATOMIC_ACQUIRE);
    uint32_t accepted = (aarch64_smp_accepted_mask() >> 1) & 1u;
    if (online != 1 || ipi_ready != 1 || sched_ok != 0 || accepted != 0) {
        log_warn("[boot-1] a64-no-ack precondition mismatch "
                 "(online=%u ipi_ready=%u scheduler_ok=%u accepted=%u)\n",
                 online, ipi_ready, sched_ok, accepted);
        return;
    }
    log_info("[boot-1] a64-no-ack cpu=1 online=1 ipi_ready=1 "
             "scheduler_ok=0 accepted=0\n");
}
#endif

void arch_boot_aps(void)
{
    uint32_t active = smp_boot_aps();
    if (active == dtb_cpu_count())
        (void)test_spinlock_smp(active);
    else
        log_warn("[spinlock] status=SKIP\n");

    extern void arch_register_subsys(void);
    arch_register_subsys();
    subsys_init_phase(SUBSYS_PHASE_4);

    if (clocksource_active) {
        kputs("[clocksource] active=true\n");
        kputs("[clocksource] freq=");
        kputu(clocksource_freq_hz());
        kputs("\n");
        kputs("[clocksource] mult=");
        kputu((uint64_t)clocksource_mult);
        kputs(" shift=");
        kputu((uint64_t)clocksource_shift);
        kputs("\n");
    }

    extern void softirq_init(void);
    softirq_init();

    if (!arch_tick_start()) {
        log_err("[smp] FATAL: BSP timer initialization failed\n");
        for (;;) arch_cpu_halt();
    }
#if OS01_SELFTEST
    gic_spi_test_init();
#endif
    log_info("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
    ipi_ready_publish_and_count(0);

    if (dtb_cpu_count() >= 2) {
        aarch64_shootdown_probe();
    } else {
        kputs("SHOOTDOWN-PROBE: SKIP (single-CPU boot)\n");
    }

#if AARCH64_SMP_TEST_NO_ACK_CPU == 1
    /* BOOT-1: exactly one runtime-qualification record, after the
     * probe and before the scheduler handoff (spec §5.3/§8.3). */
    boot1_no_ack_observation();
#endif
}

void arch_post_smp_selftest(void)
{
#if OS01_SELFTEST
    {
        extern int test_aarch64_page_table_multicore_run(void);
        (void)test_aarch64_page_table_multicore_run();
    }
#endif

    {
        uint64_t mmfr2 = 0;
        __asm__ __volatile__("mrs %0, ID_AA64MMFR2_EL1" : "=r"(mmfr2));
        kputs("[aarch64] ID_AA64MMFR2_EL1.BBM = ");
        kputu((mmfr2 >> 20) & 0xFUL);
        kputs("\n");
    }

#if OS01_SELFTEST
    kputs("[gic] dispatch ready\n");
    gic_clobber_probe();
    gic_unexpected_probe();
    gic_ipi_test(dtb_cpu_count());
#endif
}

int arch_boot_device_nodes(void)
{
    return 0;
}

void arch_start_kernel_idle(void)
{
    arch_local_irq_disable();
    aarch64_ap_sched_prepare();
    arch_aarch64_prepare_bsp_handoff();
    aarch64_bsp_stack_enter((uint64_t)&init_task_union + STACK_SIZE - 16);
    for (;;) arch_cpu_halt();
}
