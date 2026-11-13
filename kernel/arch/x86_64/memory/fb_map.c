/*
 * kernel/arch/x86_64/memory/fb_map.c
 *
 * x86_64 checked 2MiB framebuffer page mapping.
 * (Spec §2.1, Task 2)
 */

#include <arch/x86_64/fb_map.h>
#include <arch/x86_64/fb_mtrr.h>   /* MTRR raw readout + guest-side decode */
#include <arch/x86_64/msr_safe.h>    /* arch_rdmsr_safe (cache-state diagnostic) */
#include <arch/x86_64/cpuid.h>       /* cpuid (MTRR capability check) */
#include <driver/fb_cache.h>
#include <driver/fb_state.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>
#include <core/printk.h>
#include <log/log.h>               /* log_info */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef OS01_HOST_TEST
#include "fb_resolution_runtime.h"
#endif

#define FB_MAP_MAX_PAGES 64

struct fb_pmd_record {
    uint64_t *pmd;
    size_t    pmd_idx;
    uint64_t  old_val;
    bool      written;
};

static struct fb_pmd_record s_records[FB_MAP_MAX_PAGES];

#ifndef OS01_HOST_TEST
/* ── Guest-side page-table / cache-state diagnostic ───────────────────
 *
 * SCOPE (read this before quoting the numbers):
 *
 *   This prints THREE DIFFERENT things and never conflates them:
 *
 *     1. the RAW guest-visible state: IA32_PAT, IA32_MTRR_CAP,
 *        IA32_MTRR_DEF_TYPE, the fixed-range registers, and every
 *        variable range's base/mask;
 *     2. the GUEST-SIDE decode of the guest's OWN MTRR configuration
 *        for the framebuffer's physical range -- a well defined
 *        function of (1), computed by <arch/x86_64/fb_mtrr.h> with the
 *        architectural rules;
 *     3. the EFFECTIVE memory type of the accesses -- this is NOT
 *        evaluated and CANNOT be evaluated from inside the guest:
 *        under nested paging (AMD NPT / Intel EPT) the effective type
 *        is decided by the host's page tables and the host's PAT, and
 *        the guest cannot observe either of them.
 *
 *   So this diagnostic never claims that the processor used a
 *   particular memory type for the framebuffer.  It reports what the
 *   guest's own configuration says, and states explicitly that the
 *   effective type is not evaluated.
 *
 * MSR addresses (IA32 MSR register numbers, 64-bit view).  There is
 * NO "32-bit legacy alias" rule for these registers: the same number
 * is used from 32-bit and 64-bit code.  IA32_MTRR_CAP is 0x0FE in
 * 64-bit mode and 0x1FE in 32-bit mode; IA32_MTRR_DEF_TYPE is 0x2FF
 * in both modes -- 0x1FF is NOT an alias for it.
 *
 * The fixed-range registers (IA32_MTRR_FIX64R_0..3 / FIX4R_0..3)
 * describe only the lowest physical memory -- at most the lowest
 * 256 MiB on any x86-64 part (the FIX64R_0 regions are 1 MiB each on
 * the AMD family) -- while the framebuffer BAR0 sits at 2 GiB, far
 * above them, so they cannot change the decode.  Their raw values are
 * printed anyway so that this can be checked instead of assumed.
 *
 * The reads go through arch_rdmsr_safe(): a #GP from an MSR the
 * (virtual) CPU does not implement is recovered as an error instead
 * of killing the guest, so a missing capability is reported as
 * "unknown" rather than crashing the boot.
 */
static int fb_x86_read_msr(uint32_t msr, uint64_t *value, void *ctx)
{
    (void)ctx;
    return arch_rdmsr_safe(msr, value);
}

static void fb_x86_log_page_table_state(uint64_t first_leaf, uint64_t last_leaf,
                                        uint64_t pages, uint64_t pa, uint64_t size)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    bool mtrr_capable = ((edx >> 12) & 1u) != 0u;

    /* MAXPHYADDR (CPUID.0x80000008[7:0]) bounds the physical address
     * width the MTRR mask arithmetic may use. */
    unsigned max_phys = 36;
    cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000008u) {
        cpuid(0x80000008u, &eax, &ebx, &ecx, &edx);
        max_phys = (unsigned)(eax & 0xFFu);
    }

    uint64_t cr0 = 0;
    __asm__ __volatile__("movq %%cr0, %0" : "=r"(cr0));

    /* The raw MTRR snapshot is ~360 bytes.  This diagnostic is inlined
     * into fb_x86_map_checked(), where the whole struct would push the
     * mapping function's stack frame past the 512-byte stack-frame
     * audit limit.  Keep the scratch snapshot in .bss instead: the
     * diagnostic runs once, single-threaded, on the boot path, exactly
     * like the s_records rollback scratch. */
    static struct fb_mtrr_raw raw;
    fb_mtrr_read_raw(fb_x86_read_msr, NULL, max_phys, mtrr_capable, &raw);

    log_info("fb: leaf_first=0x%lx leaf_last=0x%lx pages=%lu pa=0x%lx size=0x%lx\n",
             first_leaf, last_leaf, pages, pa, size);
    log_info("fb: pat=0x%lx(%d) mtrr_capable=%d mtrr_cap=0x%lx(%d) "
             "mtrr_def_type=0x%lx(%d) cr0=0x%lx cd=%d nw=%d\n",
             raw.pat, (int)raw.pat_ok, (int)mtrr_capable,
             raw.cap, (int)raw.cap_ok, raw.def_type, (int)raw.def_ok,
             cr0, (int)((cr0 >> 30) & 1), (int)((cr0 >> 29) & 1));
    log_info("fb: mtrr raw: def_type=0x%lx E=%d FE=%d def_type_field=%s "
             "fixed64r0=0x%lx(%d) fixed4r0=0x%lx(%d) vcount=%u max_physaddr=%u\n",
             raw.def_type,
             (int)((raw.def_type & FB_MTRR_DEF_TYPE_E) ? 1 : 0),
             (int)((raw.def_type & FB_MTRR_DEF_TYPE_FE) ? 1 : 0),
             fb_mtrr_type_name(raw.def_type),
             raw.fixed64r0, (int)raw.fixed64r0_ok,
             raw.fixed4r0, (int)raw.fixed4r0_ok,
             raw.range_count, max_phys);

    for (unsigned i = 0; i < raw.range_count && i < FB_MTRR_MAX_RANGES; i++) {
        log_info("fb: mtrr raw range%u: base=0x%lx mask=0x%lx type=%s V=%d\n",
                 i, raw.base[i], raw.mask[i],
                 fb_mtrr_type_name(raw.base[i]),
                 (int)((raw.mask[i] & FB_MTRR_MASK_V) ? 1 : 0));
    }

    struct fb_mtrr_decode dec;
    fb_mtrr_decode_range(&raw, pa, size, &dec);
    if (dec.status == FB_MTRR_DECODE_UNIFORM) {
        log_info("fb: mtrr guest-decode pa=0x%lx size=0x%lx: %s "
                 "(guest's own MTRR configuration only, covering=%u)\n",
                 pa, size, fb_mtrr_type_name(dec.type), dec.covering);
    } else if (dec.status == FB_MTRR_DECODE_MIXED) {
        log_info("fb: mtrr guest-decode pa=0x%lx size=0x%lx: MIXED "
                 "(guest's own MTRR configuration only, covering=%u)\n",
                 pa, size, dec.covering);
    } else {
        log_info("fb: mtrr guest-decode pa=0x%lx size=0x%lx: UNKNOWN (%s)\n",
                 pa, size, dec.reason ? dec.reason : "no reason");
    }
    log_info("fb: effective memory type (incl. nested paging / host PAT) "
             "= NOT EVALUATED\n");
}
#endif

int fb_x86_map_checked(uint64_t phys, uint64_t size, fb_cache_policy_t policy,
                       uint32_t **out_addr)
{
    if (!out_addr) {
        return -EINVAL;
    }
    *out_addr = NULL;

    if (size == 0) {
        return -EINVAL;
    }

    /* BAR0 base and length must be 2MiB-aligned */
    if ((phys & (PAGE_2M_SIZE - 1)) != 0) {
        return -EINVAL;
    }
    if ((size & (PAGE_2M_SIZE - 1)) != 0) {
        return -EINVAL;
    }

    /* Range overflow check */
    if (phys + size < phys) {
        return -EINVAL;
    }
    if (phys + size > (UINT64_C(1) << 48)) {
        return -EINVAL;
    }

    uint64_t va_base = VIRT_FRAMEBUFFER_OFFSET;
    if (va_base + size < va_base) {
        return -EINVAL;
    }

    size_t num_pages = (size_t)(size >> PAGE_2M_SHIFT);
    if (num_pages > FB_MAP_MAX_PAGES) {
        return -EFBIG;
    }

    /* ── Framebuffer cache policy ─────────────────────────────────
     * The mapping attributes come from the policy published by the
     * identified framebuffer backend (see <driver/fb_cache.h>): the
     * QEMU Standard VGA RAM backend publishes WRITEBACK because its
     * BAR0 is ordinary host RAM, every other (generic GOP fallback)
     * stays UNCACHEABLE.
     *
     * The very same policy drives the /dev/fb user alias (fb_mmap())
     * and the early boot alias (frame_buffer_early_init()), so one
     * physical page is never accessed under two different memory types
     * -- x86 explicitly forbids that (IA32_PAT: avoid memory-type
     * aliasing).
     *
     * The candidate policy is an ARGUMENT, not a global read: the
     * mapping and the policy are published as ONE transaction.  The
     * global policy is committed only after every leaf entry is
     * installed AND verified; on any failure this call leaves both the
     * page table and the global policy exactly as it found them, so a
     * failed probe can never leave the /dev/fb alias with a memory type
     * that contradicts the kernel mapping.
     *
     * The host regression asserts the resulting page-table bits against
     * its own hard-coded expectation; it does not reuse this
     * expression. */
    uint64_t pmd_flags = fb_x86_kernel_pmd_flags(policy);

    struct fb_pmd_record *records = s_records;
    memset(records, 0, num_pages * sizeof(records[0]));
    size_t i = 0;
    int rc = 0;

    for (i = 0; i < num_pages; i++) {
        uint64_t va = va_base + ((uint64_t)i * PAGE_2M_SIZE);
        uint64_t pa = phys + ((uint64_t)i * PAGE_2M_SIZE);

        size_t pgd_idx = (size_t)(va >> PAGE_PGD_SHIFT) & 0x1ff;
        size_t pud_idx = (size_t)(va >> PAGE_1G_SHIFT) & 0x1ff;
        size_t pmd_idx = (size_t)(va >> PAGE_2M_SHIFT) & 0x1ff;

        uint64_t next_pa = 0;
        rc = vmm_get_next_level_checked(kernel_map, pgd_idx, PAGE_KERNEL_PGD, &next_pa);
        if (rc != 0) {
            goto rollback;
        }

        uint64_t *pud = (uint64_t *)Phy_To_Virt(next_pa);
        rc = vmm_get_next_level_checked(pud, pud_idx, PAGE_KERNEL_PUD, &next_pa);
        if (rc != 0) {
            goto rollback;
        }

        uint64_t *pmd = (uint64_t *)Phy_To_Virt(next_pa);
        records[i].pmd = pmd;
        records[i].pmd_idx = pmd_idx;
        records[i].old_val = pmd[pmd_idx];
        records[i].written = false;

        uint64_t flags = pmd_flags;
        pmd[pmd_idx] = x86_pte_2m_make(pa, flags);
        records[i].written = true;

#ifdef OS01_HOST_TEST
        if (g_mock_corrupt_pmd_entry) {
            /* Corrupt BOTH the PAT bit (bit 12) and the lowest physical
             * address bit of a 2MiB leaf (bit 21).  Bit 12 alone is the
             * 2MiB leaf's PAT bit -- NOT part of the physical address --
             * so a PA-only corruption has to flip bit 21 or higher. */
            pmd[pmd_idx] ^= (0x1000ULL | 0x200000ULL);
        }
#endif
    }

    /* ── Verification phase: verify every PMD's phys and cache flags ── */
    for (i = 0; i < num_pages; i++) {
        uint64_t pa = phys + ((uint64_t)i * PAGE_2M_SIZE);
        uint64_t expected = x86_pte_2m_make(pa, pmd_flags);

        uint64_t actual = records[i].pmd[records[i].pmd_idx];
        if (actual != expected) {
            rc = -EIO;
            goto rollback;
        }
    }

    /* All entries verified: the mapping is complete.  Publish the
     * policy TOGETHER with the mapping (transaction commit), then flush
     * the TLB / broadcast the shootdown.  From this point on the global
     * policy describes exactly the attribute the kernel mapping uses. */
    fb_cache_policy_set(policy);
    tlb_shootdown();
    *out_addr = (uint32_t *)va_base;

#ifndef OS01_HOST_TEST
    fb_x86_log_page_table_state(records[0].pmd[records[0].pmd_idx],
                                 records[num_pages - 1].pmd[records[num_pages - 1].pmd_idx],
                                 (uint64_t)num_pages, phys, size);
#endif
    return 0;

rollback:
    /* Restore every entry this call wrote, back to its previous value.
     * The loop must run over ALL installed pages, not over the index
     * where the verification failed: the verification loop reuses the
     * same index variable, so `i` no longer counts the written pages
     * here.  records[j].written tells which entries were touched. */
    for (size_t j = 0; j < num_pages; j++) {
        if (records[j].written && records[j].pmd) {
            records[j].pmd[records[j].pmd_idx] = records[j].old_val;
        }
    }
    tlb_shootdown();
    *out_addr = NULL;
    return rc;
}

/* ── Early framebuffer alias revocation ─────────────────────────────
 *
 * frame_buffer_early_init() maps the framebuffer physical pages a second
 * time at VIRT_FRAMEBUFFER_EARLY, writing into the boot page table.
 * vmm_init() adopts that very page table as the kernel map, so the early
 * alias is still reachable after the permanent mapping has been installed.
 *
 * Two aliases of the same physical pages are only harmless while they
 * carry the SAME memory type; once the permanent mapping goes write-back
 * while the early alias stays uncacheable, the same page is accessed
 * under two different memory types -- exactly what IA32_PAT forbids
 * ("avoid memory-type aliasing": the same physical memory must not be
 * accessed with different memory types, the processor may not guarantee
 * coherency between them).
 *
 * Revoking the alias is therefore the fix, not "nobody uses it any more":
 * the entries are cleared, so the early range is simply not mapped.
 */
int fb_x86_revoke_early_alias(uint64_t *pmd_table, uint64_t va_base, uint64_t size)
{
    if (!pmd_table || size == 0) {
        return -EINVAL;
    }

    int revoked = 0;
    for (uint64_t off = 0; off < size; off += PAGE_2M_SIZE) {
        size_t idx = (size_t)((va_base + off) >> PAGE_2M_SHIFT) & 0x1ff;
        if (pmd_table[idx] != 0) {
            pmd_table[idx] = 0;
            revoked++;
        }
    }

    if (revoked > 0) {
        /* The cleared entries must not survive in any TLB. */
        flush_tlb();
    }
    return revoked;
}

/* ── Ordered switch from the early alias to the permanent mapping ────
 *
 * See the declaration in <arch/x86_64/fb_map.h> for the contract: the
 * permanent mapping is published to the coordinator FIRST, the early
 * alias is revoked SECOND, and the revocation is skipped while a writer
 * lease is outstanding.
 */
int fb_x86_publish_and_revoke_early_alias(uint32_t *perm_addr, uint64_t perm_size,
                                           uint64_t *early_pmd, uint64_t early_va,
                                           uint64_t early_size)
{
    if (!perm_addr || !early_pmd || early_size == 0) {
        return -EINVAL;
    }

    /* 1. Publish the permanent mapping.  From here on every writer lease
     *    is granted the permanent address. */
    fb_publish_initial_mapping(perm_addr, perm_size);

    /* 2. Revoke the early alias only when no lease can still hold it. */
    if (fb_active_writers_count() != 0) {
        return 0;
    }

    return fb_x86_revoke_early_alias(early_pmd, early_va, early_size);
}
