/* UEFI-only AArch64 BSP entry. APs enter secondary_idle independently. */
#include <stdint.h>
#include <string.h>
#include <arch/boot_memory.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/page_table_selftest.h>
#include <core/bootinfo.h>
#include <log/log.h>      /* for log_err/log_info macros */
#include <core/printk.h>  /* for serial_printk (selftest markers) */
#include <core/selftest.h> /* for selftest_run_all (OS01_SELFTEST builds) */
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
#include <arch/aarch64/vmm_gate.h> /* smp_starting_enter / ipi_ready_publish_and_count (M3 Task 11) */
#include <percpu/percpu.h>        /* percpu_data / num_cpus (M3 Task 11) */
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
 * Called once from aarch64_main, after arch_vmm_init returns success. */
static void aarch64_boot_fb_init(const struct boot_context *handoff)
{
    if (!(handoff->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER)) {
        kputs("[fb] no framebuffer in handoff; color_printk disabled\n");
        return;
    }

    Pos.Phy_addr    = (uint32_t *)handoff->graphics.FrameBufferBase;
    Pos.FB_length   = handoff->graphics.FrameBufferSize;
    Pos.XResolution = handoff->graphics.HorizontalResolution;
    Pos.YResolution = handoff->graphics.VerticalResolution;
    Pos.XPosition   = 0;
    Pos.YPosition   = 0;
    /* spin_init(&Pos.lock): x86_64_boot_early does this on its path;
     * aarch64 has no equivalent stage, so we own the init here. */
    spin_init(&Pos.lock);

    frame_buffer_init();
    if (Pos.FB_addr) {
        color_printk(WHITE, BLUE,
                     "[fb] aarch64 color_printk active (%ux%u)\n",
                     Pos.XResolution, Pos.YResolution);
        kputs("[fb] frame buffer remap succeed (banner above on screen)\n");
    } else {
        kputs("[fb] mapping failed; color_printk disabled\n");
    }
}

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
     * kernel/core/main.c:155-159 prelude on x86_64, but uses the
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

    /* M1 preflight: select and publish the early arena BEFORE pmm_init.
     * aarch64_early_arena_prepare() drives aarch64_early_arena_plan() (pure input validation
     * + checked metadata sizing + low-window arena selection) and, only
     * after every check has passed, sets PMMngr.start_brk = OFFSET +
     * base_pa so pmm_init() places the metadata segment at the high-half
     * alias of the arena base. On any failure the function returns a
     * negative errno; the BSP halts here with the need / available
     * diagnostic. The function does NOT write to memory outside the
     * PMM until the success path; a fatal here means no metadata was
     * touched, so the BSP halts cleanly without corrupting RAM. */
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

    pmm_init(handoff);

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
    /* M3.5 Task 24: pin kernel_map = (mmap)(pa + ARCH_PAGE_OFFSET)
     * where pa = aarch64_read_ttbr1() & AARCH64_TTBR_BASE_MASK.
     * arch_boot_direct_map_init() above configured the runtime page
     * tables and (via aarch64_install_ttbr1()) installed the M1
     * root into TTBR1_EL1; arch_vmm_init reads TTBR1 back, validates
     * the masked PA (nonzero, 4 KiB aligned, < 1 TiB), and publishes
     * the same kernel_map pointer every downstream arch_vmm_* entry
     * expects. The M1 root remains live until the next translation
     * table change; arch_vmm_init just locates and re-publishes it.
     *
     * The call runs BEFORE aarch64_page_table_probe_prepare() and BEFORE the
     * selftest_run_all() block below so the kernel_map assertion in
     * the M3 selftest sees the live state.
     *
     * Any non-zero rc is fatal — the BSP halts here with a numeric
     * reason; v1 review item 16 forbids returning to the caller
     * because every subsequent page-table-touching path assumes
     * kernel_map is pinned. */
    {
        int rc = arch_vmm_init();
        if (rc) {
            kputs("FATAL: arch_vmm_init rc=-");
            kputu((uint64_t)(-(int64_t)rc));
            kputs("\n");
            for (;;) arch_cpu_halt();
        }
    }
    /* NEW (spec 2026-10-06): populate Pos from bootctx->graphics and map
     * the framebuffer MMIO via frame_buffer_init (in
     * kernel/arch/aarch64/runtime/printk_fb.c). color_printk becomes
     * live as a result; PL011 remains the canonical boot console (every
     * kputs/serial_printk still goes to PL011; the banner below is the
     * first output that ALSO lands on the screen).
     *
     * Called BEFORE selftest_run_all / dtb_init / gic_init / smp_boot_aps
     * so APs see the live Pos via SMP cache coherence. APs only READ
     * Pos (color_printk from kernel/memory/slab.c error paths); they
     * never write to it. */
    aarch64_boot_fb_init(handoff);
    if (aarch64_page_table_probe_prepare()) {
        log_err("M1 FATAL reason=probe\n");
        for (;;) arch_cpu_halt();
    }

#if defined(OS01_SELFTEST)
    /* M2 Task 6: run the built-in kernel selftests (selftest.c) on the
     * BSP after PMM/slab are up — mirrors kernel/core/main.c on x86_64.
     * The aarch64 registration set is portable-only (see the
     * __aarch64__ guards in selftest.c); prints the harness-asserted
     * '[selftest] slab: 16/16 PASS' marker among others. Placement:
     * after M1 runtime init / probe prepare, before DTB/GIC/SMP setup,
     * so the sync-fault probe block below stays the last pre-SMP
     * activity. */
    {
        int failed = selftest_run_all();
        serial_printk("[selftest] done (failed=%d)\n", failed);
    }
#endif

#if AARCH64_SYNC_FAULT_TEST
    /* AAGU-EL1-sync (spec §5): a controlled EL1h sync fault probe.
     * Runs on the BSP after the PMM/page-table selftests have torn
     * down their temporary mapping and BEFORE DTB/GIC/SMP setup, so
     * the only side effect is the expected data abort. We:
     *   1. Derive the active TTBR root the way aarch64_pt_smoke_test()
     *      does (validate the raw TTBR0_EL1 and convert to a
     *      direct-map pointer); any malformed TTBR halts here.
     *   2. Require AARCH64_PT_SELFTEST_VA to be absent (the smoke
     *      test unmap leaves it ENOENT). Anything else is fatal.
     *   3. Print `[aarch64-sync-test] armed` exactly once.
     *   4. Perform a volatile inline-asm `ldr` from that VA with a
     *      register output (must not be optimized away).
     *   5. Print `[aarch64-sync-test] returned` directly after.
     * The `returned` marker MUST be unreachable — the data abort
     * fires on the inline ldr and Task 1's fatal path halts the CPU
     * before any later instruction can run. If the harness ever sees
     * `returned`, the fault did not fire and the test fails.
     *
     * This block is gated by AARCH64_SYNC_FAULT_TEST (set only on
     * the dedicated sync-fault variant); production and ordinary
     * selftest images never see it. */
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
        /* Volatile inline-asm ldr from the absent VA. The fault is
         * expected to fire on this instruction; the compiler must NOT
         * reorder or elide the load, and the register output forces
         * the assembler to emit the read. */
        uint64_t probed;
        __asm__ __volatile__(
            "ldr %0, [%2]\n\t"
            : "=r"(probed)
            : "m"(*(volatile uint64_t *)(uintptr_t)AARCH64_PT_SELFTEST_VA),
              "r"((uint64_t)(uintptr_t)AARCH64_PT_SELFTEST_VA)
            : "memory");
        /* Unreachable in the passing test path. */
        kputs("[aarch64-sync-test] returned\n");
        (void)probed;
        for (;;) arch_cpu_halt();
    }
#endif

    /* Invalid or missing platform information is FATAL here, before any
     * GIC or PSCI access. Only valid platforms can degrade and keep ticks. */
    dtb_init(handoff);
    log_info("OS01 aarch64 uefi handoff ok\n");
    log_info("OS01 aarch64 phase1 boot ok\n");
    gic_init();

    /* M3 (Task 11): publish BSP double-state BEFORE the first possible
     * PSCI CPU_ON. percpu_install_gs/percpu_init are pure memory setup
     * with no hardware dependency; doing them here closes the window
     * where an AP is already running while the BSP has no runtime
     * percpu_t and online==0. percpu_init now also sets online with a
     * release store (M3.5 Task 25 fix); this BSP store is redundant
     * but kept as belt-and-braces for the pre-AP window. num_cpus
     * gates the TLB shootdown IPI loop
     * (kernel/memory/tlb.c), so it must also be visible before any AP
     * can run. smp_starting_enter() latches the one-way SMP gate: from
     * here on vmm_gate_check() refuses a VMM change until every DTB CPU
     * has published ipi_ready. */
    {
        extern void percpu_install_gs(uint32_t cpu);
        extern void percpu_init(uint32_t cpu, uint32_t apic_id);
        uint32_t mpidr_bsp;
        __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_bsp));
        percpu_install_gs(0);
        percpu_init(0, mpidr_bsp);
        __atomic_store_n(&percpu_data[0].online, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE);
        smp_starting_enter();
    }

    uint32_t active = smp_boot_aps();
    if (active == dtb_cpu_count())
        (void)test_spinlock_smp(active);
    else
        log_warn("[spinlock] status=SKIP\n");

    /* BSP-only timer and IRQs begin after AP startup/testing has settled. */
    /* SUBSYS_INITCALL Task 2 — register+dispatch pair (R3-1 critical).
     * Mirror of x86_64 kernel/core/main.c:193-194. Without the second
     * call, no SUBSYS_INITCALL-registered initcall ever runs.
     *
     * - arch_register_subsys() iterates the .subsys_init linker range
     *   and calls each queued _register wrapper, which populates
     *   subsys_table[] via register_subsys().
     * - subsys_init_phase(SUBSYS_PHASE_4) walks subsys_table[] and
     *   invokes every registered init wrapper for phase 4 — after
     *   Task 3's gate flip on clocksource.c landed, this is the path
     *   that actually calls clocksource_init(). Group 3b (this commit)
     *   removed the previous explicit Option B fallback so the
     *   dispatch path is the single source of invocation.
     *
     * arch_register_subsys() is declared via <arch/subsys.h>... NOT
     * pulled in here (R3-3 NIT: avoid <arch/subsys.h> transitively),
     * so we forward-declare it locally. subsys_init_phase() and the
     * SUBSYS_PHASE_4 macro come from <subsys/subsys.h> (added above). */
    extern void arch_register_subsys(void);
    arch_register_subsys();
    subsys_init_phase(SUBSYS_PHASE_4);

    /* aarch64 Generic Timer Task 2.2 GREEN — emit the three
     * [clocksource] markers required by
     * qemutests/aarch64_uefi_smp.py --expect-clk (clk_evidence_ok
     * asserts exactly one active=true, one freq=<N>, one mult= shift=).
     * We print the markers here from main.c (instead of from inside
     * clocksource_init()) so the kernel TU here avoids
     * `#include <time/clocksource.h>`, which transitively pulls in
     * <list.h> via <time/timer.h> — see the forward-decl block above.
     *
     * The block must come AFTER `subsys_init_phase(SUBSYS_PHASE_4)`
     * above and BEFORE `arch_tick_start()` below so:
     *   1. `clocksource_init()` has already been dispatched, so
     *      `clocksource_active == true` and the markers print.
     *   2. The markers appear in the QEMU stdout.log BEFORE the
     *      `[cntp]` marker emitted by arch_tick_start(), satisfying
     *      the harness evidence gate. */
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

    /* softirq_init() must run BEFORE arch_tick_start(): tick_handler()
     * calls set_softirq_status(TIMER_SIRQ), which dereferences
     * softirq_status (BSS; zeroed). softirq_init() clears the
     * softirq vector and status.
     *
     * On x86_64, softirq_init() is called from kernel/intr/irq.c:78
     * (existing). aarch64 has no irq.c — explicit call needed. */
    extern void softirq_init(void);
    softirq_init();

    /* Phase 2 #3: BSP percpu data is now installed before smp_boot_aps()
     * (M3 Task 11) — see the block above the SMP bring-up call. */

    if (!arch_tick_start()) {
        log_err("[smp] FATAL: BSP timer initialization failed\n");
        for (;;) arch_cpu_halt();
    }
#if OS01_SELFTEST
    /* Task 2.3b — arm the PL011 RX SPI handler BEFORE IRQ unmask, so a
     * pending SPI cannot fire into an empty handler table. */
    gic_spi_test_init();
#endif
    log_info("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
    /* M3 (Task 11): publish BSP ipi_ready through the same one-shot
     * function as the APs — only AFTER the tick handler is registered
     * (arch_tick_start above), the TLB SGI handler is live (gic_init)
     * and DAIF.I is unmasked. Publishing earlier would let a pending
     * TLB SGI arrive with IRQs masked (no ack → initiator timeout). */
    ipi_ready_publish_and_count(0);

    /* M3.5 Task 25: production shootdown probe (spec §7.3).
     *
     * Runs AFTER BSP ipi_ready is published and AFTER smp_boot_aps()
     * has returned (all APs gone through the boot handshake and into
     * secondary_idle). The probe waits inside for ipi_ready_count
     * to reach dtb_cpu_count() — APs publish ipi_ready only AFTER
     * opening IRQs + ISB (secondary_idle tail, Task 10 Step 4), so a
     * poll is required.
     *
     * The probe runs in production AND selftest builds. Single-CPU
     * boots (-smp 1) skip it via the dtb_cpu_count() >= 2 gate —
     * the probe body's `requires-at-least-one-AP` FAIL path would
     * otherwise halt the system on every -smp 1 selftest run,
     * breaking MODE=smp 1/2/4. The body itself still owns the
     * FAIL contract (hosttest pins the 0-AP path); production and
     * selftest images at -smp >= 2 reach the full 7-step sequence
     * and either print "SHOOTDOWN-PROBE: OK" or halt. */
    if (dtb_cpu_count() >= 2) {
        aarch64_shootdown_probe();
    } else {
        kputs("SHOOTDOWN-PROBE: SKIP (single-CPU boot)\n");
    }

#if OS01_SELFTEST
    /* M3.6 Task 26: multi-core selftest (spec §8.2 ①②⑤⑥) — real-core
     * shootdown target-set/gen, partial-mask SGI, same-pt_lock
     * interleave and concurrent shared-L1 creation. MUST run post-SMP
     * (APs in secondary_idle + ipi_ready), i.e. NOT through the
     * pre-SMP selftest_run_all() table. Prints the harness-asserted
     * '[selftest] m3mc: N/N PASS' marker (SKIP line at -smp 1). */
    {
        extern int test_aarch64_page_table_multicore_run(void);
        (void)test_aarch64_page_table_multicore_run();
    }
#endif

    /* M3.4 Task 21: ID_AA64MMFR2_EL1.BBM (Break-before-Make levels).
     *
     * The split_block_2m path on a PUBLISHED root depends on the BBM
     * support level reported here — levels 0/1/2 describe how much
     * BBM flexibility the CPU provides for block↔table descriptor
     * replacement.  M3.4 returns -EPERM for published-root splits
     * because the spec scopes the real implementation to F10; this
     * print lets F10 cite the actual level when it lands.
     *
     * IMPORTANT (F10 caveat): cortex-a53 / QEMU passing this print
     * does NOT replace the architectural prerequisite — the F10
     * design must reconcile the value against Arm ARM (and against
     * each real target silicon) before relying on BBM. */
    {
        uint64_t mmfr2 = 0;
        __asm__ __volatile__("mrs %0, ID_AA64MMFR2_EL1" : "=r"(mmfr2));
        kputs("[aarch64] ID_AA64MMFR2_EL1.BBM = ");
        kputu((mmfr2 >> 20) & 0xFUL);
        kputs("\n");
    }

#if OS01_SELFTEST
    /* Task 2.2 — dispatch chain selftest probes.
     * VBAR is installed (line 188-189); handler table is populated
     * (arch_tick_start registered cntp_tick_handler); dispatch is
     * live (gic.c::gic_init ran).  The probes fire a deterministic
     * SGI 2 (clobber probe) and a SPI 40 (unexpected probe) and
     * require IRQs to be unmasked above. */
    kputs("[gic] dispatch ready\n");
    gic_clobber_probe();
    gic_unexpected_probe();
    gic_ipi_test(dtb_cpu_count());
#endif
    for (;;) arch_cpu_halt();
}
