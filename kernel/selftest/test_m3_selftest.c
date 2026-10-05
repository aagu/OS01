// kernel/selftest/test_m3_selftest.c —
// aarch64 M3 selftest (aarch64 M3.4, Task 22).
//
// Exercises the four aarch64_pt_* primitives the M3.3/M3.4 backend
// (arch_vmm_*) builds on, plus the gic_target_bit cache that the
// ITARGETSR0 probe populates on SMP bring-up. Runs from
// kernel/selftest/selftest.c::selftest_run_all() on the BSP
// pre-SMP (before smp_starting_enter()), where vmm_gate_check is
// a no-op (Task 7 §6.3 pre-SMP例外) and any private root is
// by definition unpublished.
//
// Sections (each prints a per-section PASS marker; the parser
// gates on the final summary line "[selftest] m3: N/N PASS"):
//
//   (1) gic_target_bit cache (RAZ/WI + multi-core semantics).
//       Verifies the same uint8_t cache gic_target_bit_init writes
//       via either path (-smp 1: RAZ/WI例外 sets bit[0]=1u;
//       -smp 2/4: probe reads GICD_ITARGETSR0). The selftest
//       calls gic_target_bit_inject (hosttest-friendly mirror)
//       for the BSP cpu and verifies gic_target_bit_get(0). The
//       real gic_target_bit_init() invocation is exercised by
//       the existing post-SMP ipi_broadcast probe in main.c.
//
//   (2) map_4k_new + update_4k + unmap_4k + query_4k round on a
//       kernel-internal SCRATCH root. The SCRATCH root is a
//       fresh 4 KiB alloc that the test authors and frees — not
//       the published M1 root — so all four primitives exercise
//       the post-M3.3 backend code path without touching the
//       live TTBR1 root.
//
//   (3) map_2m_block + unmap_2m_block on the SCRATCH root.
//       Same shape as (2) at the 2 MiB granularity.
//
//   (4) split_block_2m on the SCRATCH root (unpublished → allowed).
//       This is the path that returns -EPERM on a published root
//       (spec §5.3, Task 21); the SCRATCH root bypasses the
//       published-root registry check so the L2 block→L3 table
//       rewrite actually runs.
//
// Scratch-VA choice: SCRATCH_BASE_VA = 0xffff_ffff_0000_0000 lands
// at PGD[511], PUD[0] in the 48-bit AArch64 VA layout. Bits
// [47:39] = 0x1FF (= 511), bits [38:30] = 0. So every scratch VA
// in the test lives under PGD[511] → PUD[0]. The L2 (PMD) index
// separates 4K VAs (PMD[0]) from the 2M VA (PMD[1]).
//
//   M3_SCRATCH_VA_4K     = 0xffff_ffff_0000_0000  (PMD[0], PTE[0])
//   M3_SCRATCH_VA_4K_2   = 0xffff_ffff_0000_1000  (PMD[0], PTE[1])
//   M3_SCRATCH_VA_4K_3   = 0xffff_ffff_0000_2000  (PMD[0], PTE[2])
//   M3_SCRATCH_VA_2M     = 0xffff_ffff_0020_0000  (PMD[1])
//
// Since the SCRATCH root is private (never installed into TTBR),
// these VAs are arbitrary well-formed values — the hardware never
// translates through the scratch tree.
//
// Whitelist: kernel/Makefile aarch64 KERNEL_C_SOURCES
// (kernel/selftest/test_m3_selftest.c). nm kernel.elf | grep
// test_m3 must be non-empty before commit.
//
// x86_64: the entire body is wrapped in #ifdef __aarch64__ +
// OS01_SELFTEST, matching test_aarch64_rndr_encoding.c.

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/boot_direct_map.h>  /* aarch64_read_ttbr1 (M3.5 Task 24) */
#include <arch/aarch64/boot_log.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <core/printk.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>                    /* kernel_map + mmap type (M3.5 Task 24) */

#ifdef OS01_SELFTEST

#if defined(__aarch64__)

/* ── Scratch VA / PA constants ──────────────────────────────────── */
/* All scratch VAs land at PGD[511] → PUD[0]. PMD index separates
 * 4K leaves (PMD[0]) from the 2M block (PMD[1]). */
#define M3_SCRATCH_BASE_VA   UINT64_C(0xffffffff00000000)
#define M3_SCRATCH_VA_4K     (M3_SCRATCH_BASE_VA + UINT64_C(0x00000000))
#define M3_SCRATCH_VA_4K_2   (M3_SCRATCH_BASE_VA + UINT64_C(0x00001000))
#define M3_SCRATCH_VA_4K_3   (M3_SCRATCH_BASE_VA + UINT64_C(0x00002000))
#define M3_SCRATCH_VA_2M     (M3_SCRATCH_BASE_VA + UINT64_C(0x00200000))
#define M3_PGD_IDX           511u
#define M3_PUD_IDX           0u
#define M3_PMD_IDX_4K        0u
#define M3_PMD_IDX_2M        1u
#define M3_DESC_PA_MASK      UINT64_C(0x000000fffffff000)

/* Print a section-FAIL line and return the negative errno. serial_printk
 * on aarch64 is verbatim (no format substitution — see
 * kernel/arch/aarch64/runtime/serial_printk.c), so use kputs for the
 * literal parts and kputu for the signed rc value. */
static int section_fail(const char *section, int rc)
{
    /* Render rc as signed decimal. AARCH64_PT_ codes are negative
     * Linux-style errno, so they all fit in int32 — print via an
     * inline decimal converter. */
    char buf[24];
    int n = 0;
    uint64_t v = (uint64_t)(int64_t)rc;
    if (rc < 0) {
        buf[n++] = '-';
        v = (uint64_t)(-(int64_t)rc);
    }
    char tmp[24];
    int tn = 0;
    if (v == 0) {
        tmp[tn++] = '0';
    } else {
        while (v != 0) {
            tmp[tn++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (tn > 0) buf[n++] = tmp[--tn];
    buf[n] = '\0';

    kputs("[selftest] m3: ");
    kputs(section);
    kputs(" FAIL rc=");
    kputs(buf);
    kputs("\n");
    return rc;
}

/* Free the SCRATCH root's intermediates (L0→L1→L2) and the
 * scratch root itself, plus any data PAs the caller allocated.
 *
 * Walks PGD[511] → PUD[0] → PMD[0..1], frees every TABLE
 * descriptor it finds (those are intermediate pages the test
 * allocated). The 4K and 2M sections touch PMD[0] and PMD[1]
 * respectively; the split section touches PMD[1]. Walking the
 * pair covers all three. BLOCK descriptors (PMD[1] in the 2M
 * section after map_2m_block) are skipped — the block's data PA
 * lives in `data_pas[]` and is freed by the caller. */
static void scratch_root_free(uint64_t *root, const uint64_t *data_pas,
                              unsigned data_count)
{
    if (root == NULL) return;
    uint64_t l0 = root[M3_PGD_IDX];
    if ((l0 & 3) == 3) {
        uint64_t l0_pa = l0 & M3_DESC_PA_MASK;
        uint64_t *l1 = (uint64_t *)Phy_To_Virt(l0_pa);
        uint64_t l1e = l1[M3_PUD_IDX];
        if ((l1e & 3) == 3) {
            uint64_t l1e_pa = l1e & M3_DESC_PA_MASK;
            uint64_t *l2 = (uint64_t *)Phy_To_Virt(l1e_pa);
            /* Walk the full L2: any section may have populated any
             * PMD slot, and a split_block_2m installs an L3 table at
             * the split slot. Unused entries are 0 and skipped, so
             * the full 512-entry sweep is safe (Task 22 follow-up:
             * the old j < 2 bound only covered PMD[0..1]). */
            for (unsigned j = 0; j < 512u; j++) {
                uint64_t l2e = l2[j];
                if ((l2e & 3) == 3)
                    free_4k_page(l2e & M3_DESC_PA_MASK);
            }
            free_4k_page(l1e_pa);
        }
        free_4k_page(l0_pa);
    }
    root[M3_PGD_IDX] = 0;
    free_4k_page((uint64_t)((uintptr_t)root - ARCH_PAGE_OFFSET));
    for (unsigned k = 0; k < data_count; k++)
        free_4k_page(data_pas[k]);
}

/* ── Section 1: GIC target bit cache ────────────────────────────── */
static int section_gic_target_bit(void)
{
    /* Spec §6.3: gic_target_bit_init has two paths:
     *   - dtb_cpu_count() == 1  → RAZ/WI例外:  gic_target_bit[0] = 1u
     *   - dtb_cpu_count() >  1  → ITARGETSR0 probe → 1u << cpu_id
     * Both write the same uint8_t cache this section exercises.
     * The selftest runs pre-dtb_init/pre-gic_init, so calling the
     * real function would read from an unmapped GIC region; we use
     * the hosttest-friendly inject/get pair to confirm the cache
     * is wired up. The real function runs during smp_boot_aps
     * (kernel/arch/aarch64/smp/smp.c:268) and is covered by the
     * existing ipi_broadcast probe in main.c. */
    gic_target_bit_inject(0, 1u);
    uint8_t got = gic_target_bit_get(0);
    if (got != 1u)
        return section_fail("gic_target_bit", -EIO);
    /* Out-of-range cpu must read back as 0 (test the bounds guard
     * here rather than calling the real function pre-dtb). */
    if (gic_target_bit_get(AARCH64_BOOT_MAX_CPUS) != 0)
        return section_fail("gic_target_bit:oor", -EIO);
    if (gic_target_bit_get(AARCH64_BOOT_MAX_CPUS + 7) != 0)
        return section_fail("gic_target_bit:oor2", -EIO);
    kputs("[selftest] m3: gic_target_bit PASS\n");
    return 0;
}

/* ── Section 2: 4 KiB map/update/unmap/query round on scratch root ─ */
static int section_4k_round(uint64_t *root)
{
    uint64_t data = alloc_4k_page();
    if (data == 0) return section_fail("4k:alloc-data", -ENOMEM);
    uint64_t data_pas[1] = { data };

    /* (a) map_4k_new: empty scratch root, fresh 4K install. */
    int rc = aarch64_pt_map_4k_ext(root, M3_SCRATCH_VA_4K, data,
                                   AARCH64_PT_KERNEL_RW, 0);
    if (rc != AARCH64_PT_OK) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:map_new", rc);
    }

    /* (b) query_4k: round-trip the leaf we just installed. */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    uint64_t qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, M3_SCRATCH_VA_4K,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != data ||
        qperm != AARCH64_PT_KERNEL_RW || qsw != 0) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:query", rc ? rc : -EIO);
    }

    /* (c) update_4k: rewrite to RO, verify old was RW. */
    uint64_t old_pa = 0;
    uint32_t old_perm = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, M3_SCRATCH_VA_4K, data,
                               AARCH64_PT_KERNEL_RO, 0,
                               &old_pa, &old_perm, &old_sw);
    if (rc != AARCH64_PT_OK || old_pa != data ||
        old_perm != AARCH64_PT_KERNEL_RW || old_sw != 0) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:update", rc ? rc : -EIO);
    }
    /* Re-read after update: must be RO now. */
    qpa = qperm = qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, M3_SCRATCH_VA_4K,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != data ||
        qperm != AARCH64_PT_KERNEL_RO) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:query_after_update", rc ? rc : -EIO);
    }

    /* (d) unmap_4k: clear the leaf, verify prior state. */
    uint64_t u_pa = 0;
    uint32_t u_perm = 0;
    uint64_t u_sw = 0;
    rc = aarch64_pt_unmap_4k_ext(root, M3_SCRATCH_VA_4K,
                                 &u_pa, &u_perm, &u_sw);
    if (rc != AARCH64_PT_OK || u_pa != data ||
        u_perm != AARCH64_PT_KERNEL_RO || u_sw != 0) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:unmap", rc ? rc : -EIO);
    }
    /* Re-query: must be absent now. */
    qpa = qperm = qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, M3_SCRATCH_VA_4K,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_ENOENT) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:query_after_unmap",
                            rc ? rc : -EIO);
    }

    /* (e) EEXIST contract: re-map succeeds, then second map fails
     * with -EEXIST (slot now occupied). */
    rc = aarch64_pt_map_4k_ext(root, M3_SCRATCH_VA_4K, data,
                               AARCH64_PT_KERNEL_RW, 0);
    if (rc != AARCH64_PT_OK) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:remap_new", rc);
    }
    rc = aarch64_pt_map_4k_ext(root, M3_SCRATCH_VA_4K, data,
                               AARCH64_PT_KERNEL_RW, 0);
    if (rc != AARCH64_PT_EEXIST) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:eexist", rc ? rc : -EIO);
    }
    /* Clean up: unmap before the next section. */
    rc = aarch64_pt_unmap_4k_ext(root, M3_SCRATCH_VA_4K,
                                 &u_pa, &u_perm, &u_sw);
    if (rc != AARCH64_PT_OK) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("4k:final_unmap", rc);
    }

    scratch_root_free(root, data_pas, 1);
    kputs("[selftest] m3: 4k_round PASS\n");
    return 0;
}

/* ── Section 3: 2 MiB block map / unmap round on scratch root ──── */
static int section_2m_block(uint64_t *root)
{
    /* 2 MiB block mapping requires a 2 MiB-aligned PA — claim a
     * fresh 2 MiB page from the PMM (alloc_pages returns a Page*
     * for a 2 MiB-aligned frame). */
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) return section_fail("2m:alloc", -ENOMEM);
    uint64_t block_pa = pg->phy_address;
    if ((block_pa & (PAGE_2M_SIZE - 1)) != 0) {
        free_pages(pg, 1);
        return section_fail("2m:align", -EINVAL);
    }

    uint64_t data_pas[1] = { block_pa };

    int rc = aarch64_pt_map_2m_block(root, M3_SCRATCH_VA_2M,
                                    block_pa, AARCH64_PT_KERNEL_RW);
    if (rc != AARCH64_PT_OK) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("2m:map", rc);
    }
    /* EEXIST: a second install on the same VA must fail. */
    rc = aarch64_pt_map_2m_block(root, M3_SCRATCH_VA_2M,
                                block_pa, AARCH64_PT_KERNEL_RW);
    if (rc != AARCH64_PT_EEXIST) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("2m:eexist", rc ? rc : -EIO);
    }
    uint64_t u_pa = 0;
    rc = aarch64_pt_unmap_2m_block(root, M3_SCRATCH_VA_2M, &u_pa);
    if (rc != AARCH64_PT_OK || u_pa != block_pa) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("2m:unmap", rc ? rc : -EIO);
    }
    /* ENOENT: a second unmap must fail. */
    rc = aarch64_pt_unmap_2m_block(root, M3_SCRATCH_VA_2M, &u_pa);
    if (rc != AARCH64_PT_ENOENT) {
        scratch_root_free(root, data_pas, 1);
        return section_fail("2m:enoent", rc ? rc : -EIO);
    }
    scratch_root_free(root, data_pas, 1);
    kputs("[selftest] m3: 2m_block PASS\n");
    return 0;
}

/* ── Section 4: split_block_2m on the SCRATCH root ─────────────── */
static int section_split_2m(uint64_t *root)
{
    /* Fresh scratch root so the free helper at the end has only
     * what THIS section allocated to release. */
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) return section_fail("split:alloc_block", -ENOMEM);
    uint64_t block_pa = pg->phy_address;
    if ((block_pa & (PAGE_2M_SIZE - 1)) != 0) {
        free_pages(pg, 1);
        return section_fail("split:align", -EINVAL);
    }

    /* Install a 2 MiB block, then split it into 512 4 KiB leaves. */
    int rc = aarch64_pt_map_2m_block(root, M3_SCRATCH_VA_2M,
                                    block_pa, AARCH64_PT_KERNEL_RW);
    if (rc != AARCH64_PT_OK) {
        free_pages(pg, 1);
        scratch_root_free(root, NULL, 0);
        return section_fail("split:map_2m", rc);
    }
    /* The scratch root is unpublished (we never published it via
     * aarch64_pt_root_publish) — the split path MUST succeed and
     * MUST NOT return -EPERM. */
    rc = aarch64_pt_split_block_2m(root, M3_SCRATCH_VA_2M);
    if (rc != AARCH64_PT_OK) {
        free_pages(pg, 1);
        scratch_root_free(root, NULL, 0);
        return section_fail("split:block_2m", rc);
    }
    /* After the split, the L2 slot is a table descriptor pointing
     * at the freshly-allocated L3 page. Query a leaf that lives
     * INSIDE the just-split block (L2 index = 1 = M3_PMD_IDX_2M),
     * at the first 4 KiB page of the block. The split wrote leaf
     * (block_pa + 0*PAGE_4K_SIZE) at L3[0]; querying VA
     * M3_SCRATCH_VA_2M (= SCRATCH_VA_2M_BASE + 0) lands at L3[0]
     * and must report block_pa + 0 as the PA. */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    uint64_t qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, M3_SCRATCH_VA_2M,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != block_pa ||
        qperm != AARCH64_PT_KERNEL_RW) {
        free_pages(pg, 1);
        scratch_root_free(root, NULL, 0);
        return section_fail("split:query_first_leaf",
                            rc ? rc : -EIO);
    }
    /* A leaf one page into the block must report block_pa + 4K. */
    qpa = qperm = qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, M3_SCRATCH_VA_2M + PAGE_4K_SIZE,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != block_pa + PAGE_4K_SIZE ||
        qperm != AARCH64_PT_KERNEL_RW) {
        free_pages(pg, 1);
        scratch_root_free(root, NULL, 0);
        return section_fail("split:query_second_leaf",
                            rc ? rc : -EIO);
    }
    /* And the third 4 KiB leaf in the block. */
    qpa = qperm = qsw = 0;
    rc = aarch64_pt_query_4k_ext(root,
                                 M3_SCRATCH_VA_2M + 2 * PAGE_4K_SIZE,
                                 &qpa, &qperm, &qsw);
    if (rc != AARCH64_PT_OK || qpa != block_pa + 2 * PAGE_4K_SIZE) {
        free_pages(pg, 1);
        scratch_root_free(root, NULL, 0);
        return section_fail("split:query_third_leaf",
                            rc ? rc : -EIO);
    }

    free_pages(pg, 1);
    scratch_root_free(root, NULL, 0);
    kputs("[selftest] m3: split_2m PASS\n");
    return 0;
}

/* ── Section 5: kernel_map pinned to TTBR1's direct-map pointer ──
 *
 * Spec §4.5: arch_vmm_init() pins kernel_map =
 *     (mmap)(uintptr_t)((aarch64_read_ttbr1() & AARCH64_TTBR_BASE_MASK)
 *                       + ARCH_PAGE_OFFSET)
 *
 * This section is the on-target companion to hosttests/cases/
 * test_aarch64_arch_vmm_init.c. The production call site
 * (kernel/arch/aarch64/boot/main.c) invokes arch_vmm_init() between
 * arch_boot_direct_map_init() and aarch64_m1_probe_prepare(), so the
 * live TTBR1 already holds the M1 root PA when this section runs.
 *
 * Reads the live TTBR1, masks with AARCH64_TTBR_BASE_MASK, forms the
 * expected direct-map pointer, and asserts the kernel_map global
 * matches. A drift means either arch_vmm_init produced the wrong
 * pointer or the BSP has changed TTBR1_EL1 between the production
 * call and this selftest — both are fatal at this point in the boot
 * (the GIC and TLB paths depend on the pinned root).
 *
 * Pre-SMP gate is implicitly satisfied (BSP-only, no IPI yet). */
static int section_kernel_map_pinned(void)
{
    uint64_t raw = aarch64_read_ttbr1();
    uint64_t ttbr_pa = raw & AARCH64_TTBR_BASE_MASK;
    /* Defensive pre-checks: same edge cases the production call site
     * validated before pinning kernel_map. A drift here would mean
     * somebody flipped TTBR1_EL1 between arch_vmm_init() and this
     * section — halt with a reason rather than papering over with
     * -EIO. */
    if (ttbr_pa == 0
        || (ttbr_pa & (PAGE_4K_SIZE - 1)) != 0
        || ttbr_pa >= (UINT64_C(1) << 40)) {
        kputs("[selftest] m3: kernel_map_pinned FAIL raw=");
        kputu(raw);
        kputs(" pa=");
        kputu(ttbr_pa);
        kputs("\n");
        return -EIO;
    }
    uint64_t *expected = (uint64_t *)(uintptr_t)(ttbr_pa + ARCH_PAGE_OFFSET);
    if (kernel_map != expected) {
        kputs("[selftest] m3: kernel_map_pinned FAIL got=");
        kputu((uint64_t)(uintptr_t)kernel_map);
        kputs(" want=");
        kputu((uint64_t)(uintptr_t)expected);
        kputs("\n");
        return -EIO;
    }
    kputs("[selftest] m3: kernel_map_pinned PASS\n");
    return 0;
}

/* ── Top-level: 5 sections, each owns its own scratch root ─────── */
int test_m3_selftest(void)
{
    int passed = 0;
    int failed = 0;

    /* Section 1 has no scratch root — it tests the global
     * gic_target_bit[] cache. */
    if (section_gic_target_bit() == 0) passed++; else failed++;

    /* Sections 2, 3, 4 each allocate a fresh scratch root (so the
     * walk at the end of each section never sees stale data from
     * a prior section). The root is never published, so every
     * aarch64_pt_* primitive in the section exercises the
     * un-published-root code path — and split_block_2m returns
     * AARCH64_PT_OK instead of -EPERM. */

    /* Section 2: 4 KiB round. */
    {
        uint64_t root_pa = alloc_4k_page();
        if (root_pa == 0) {
            failed++;
            kputs("[selftest] m3: 4k_round FAIL rc=-ENOMEM (root)\n");
        } else {
            uint64_t *root = (uint64_t *)Phy_To_Virt(root_pa);
            /* Zero the root (alloc_4k_page returns a subpage pool
             * slot whose prior contents are not guaranteed clean). */
            volatile uint64_t *cursor = root;
            for (unsigned k = 0; k < PAGE_4K_SIZE / sizeof(uint64_t); k++)
                cursor[k] = 0;
            if (section_4k_round(root) == 0) passed++; else failed++;
        }
    }

    /* Section 3: 2 MiB block round. */
    {
        uint64_t root_pa = alloc_4k_page();
        if (root_pa == 0) {
            failed++;
            kputs("[selftest] m3: 2m_block FAIL rc=-ENOMEM (root)\n");
        } else {
            uint64_t *root = (uint64_t *)Phy_To_Virt(root_pa);
            volatile uint64_t *cursor = root;
            for (unsigned k = 0; k < PAGE_4K_SIZE / sizeof(uint64_t); k++)
                cursor[k] = 0;
            if (section_2m_block(root) == 0) passed++; else failed++;
        }
    }

    /* Section 4: split_block_2m round (unpublished scratch root). */
    {
        uint64_t root_pa = alloc_4k_page();
        if (root_pa == 0) {
            failed++;
            kputs("[selftest] m3: split_2m FAIL rc=-ENOMEM (root)\n");
        } else {
            uint64_t *root = (uint64_t *)Phy_To_Virt(root_pa);
            volatile uint64_t *cursor = root;
            for (unsigned k = 0; k < PAGE_4K_SIZE / sizeof(uint64_t); k++)
                cursor[k] = 0;
            if (section_split_2m(root) == 0) passed++; else failed++;
        }
    }

    /* Section 5: kernel_map pinned to TTBR1's direct-map pointer.
     * No scratch root — reads the live TTBR1 + kernel_map and
     * compares. Runs after the arch_vmm_init() production call in
     * main.c, so a pass here proves the call site pinned the
     * expected pointer. */
    if (section_kernel_map_pinned() == 0) passed++; else failed++;

    /* Parser-asserted final marker: "[selftest] m3: N/N PASS".
     * 5 sections (gic_target_bit, 4k_round, 2m_block, split_2m,
     * kernel_map_pinned). Use kputs + kputu so the marker is
     * parseable (serial_printk is verbatim on aarch64). */
    if (failed == 0) {
        kputs("[selftest] m3: ");
        kputu((uint64_t)passed);
        kputs("/");
        kputu((uint64_t)passed);
        kputs(" PASS\n");
        return 0;
    }
    int total = passed + failed;
    kputs("[selftest] m3: ");
    kputu((uint64_t)passed);
    kputs("/");
    kputu((uint64_t)total);
    kputs(" PASS (failed=");
    kputu((uint64_t)failed);
    kputs(")\n");
    return -1;
}

#else /* !__aarch64__ */

int test_m3_selftest(void)
{
    /* x86_64 stub: the file is on the aarch64 whitelist only
     * (kernel/Makefile explicitly enumerates KERNEL_C_SOURCES for
     * aarch64), so this branch is unreachable in practice. Still
     * a safe no-op in case x86 ever pulls the file in. */
    /* Use serial_printk with no format specifiers (x86 path
     * supports formats but the marker is plain ASCII here). */
    serial_printk("[selftest] m3: 0/0 PASS\n");
    return 0;
}

#endif /* __aarch64__ */

#else /* !OS01_SELFTEST */

int test_m3_selftest(void)
{
    /* Non-selftest build: no-op so the symbol exists if some
     * production code accidentally references it. */
    return 0;
}

#endif /* OS01_SELFTEST */
