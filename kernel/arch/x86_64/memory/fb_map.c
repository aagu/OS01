/*
 * kernel/arch/x86_64/memory/fb_map.c
 *
 * x86_64 checked 2MiB framebuffer page mapping.
 * (Spec §2.1, Task 2)
 */

#include <arch/x86_64/fb_map.h>
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
 *   This prints the STATE that participates in the effective memory
 *   type of the framebuffer pages, and it derives the MTRR type that
 *   covers the BAR0 physical range.  It does NOT claim to prove the
 *   effective type of every access:
 *
 *     * the printed leaf entries are the ones this mapping installed
 *       (first and last), not a proof about the whole BAR0;
 *     * the MTRR derivation covers IA32_MTRR_DEF_TYPE plus the VARIABLE
 *       MTRR ranges (0x200-0x20F).  The fixed ranges (0xFE/0xFF) only
 *       cover the first 4 MiB of physical memory, which the framebuffer
 *       BAR0 is far above, so they cannot change the answer;
 *     * the interaction with nested paging (AMD NPT / Intel EPT) is
 *       NOT evaluated here.  Under NPT the guest PAT entry and the
 *       host page-table attributes decide the guest access; the guest
 *       side can only report its own PAT.
 *
 * MSR addresses (IA32_MSR register numbers, 64-bit view):
 *   IA32_PAT           = 0x277
 *   IA32_MTRR_CAP      = 0x0FE   (0x1FE is the 32-bit legacy alias)
 *   IA32_MTRR_DEF_TYPE = 0x2FF   (0x1FF is the 32-bit legacy alias)
 *   IA32_MTRR_BASE(n)  = 0x200 + 2n
 *   IA32_MTRR_MASK(n)  = 0x201 + 2n
 *
 * The reads go through arch_rdmsr_safe(): a #GP from an MSR the
 * (virtual) CPU does not implement is recovered as an error instead
 * of killing the guest, so a missing capability is reported as
 * "unknown" rather than crashing the boot.
 */
#define FB_IA32_PAT            0x277u
#define FB_IA32_MTRR_CAP       0x0FEu
#define FB_IA32_MTRR_DEF_TYPE  0x2FFu
#define FB_IA32_MTRR_BASE(n)   (0x200u + 2u * (n))
#define FB_IA32_MTRR_MASK(n)   (0x201u + 2u * (n))

/* MTRR type ranking: smaller rank == more restrictive.  On an overlap
 * the more restrictive type wins (Intel SDM Vol 3A §11.2.3). */
static int fb_mtrr_type_rank(uint64_t type)
{
    switch (type & 0xFFu) {
    case 0x00: return 0;  /* UC  */
    case 0x04: return 1;  /* WT  */
    case 0x05: return 2;  /* WP  */
    case 0x01: return 3;  /* WB  */
    default:   return 4;  /* unknown/reserved: treat as least restrictive */
    }
}

static const char *fb_mtrr_type_name(uint64_t type)
{
    switch (type & 0xFFu) {
    case 0x00: return "UC";
    case 0x01: return "WB";
    case 0x04: return "WT";
    case 0x05: return "WP";
    default:   return "?";
    }
}

/* Derive the MTRR type that covers [pa, pa+size).  Returns the type
 * value and sets *known to false when the state could not be read. */
static uint64_t fb_x86_mtrr_type_for(uint64_t pa, uint64_t size, bool *known)
{
    *known = false;

    uint64_t def = 0;
    if (arch_rdmsr_safe(FB_IA32_MTRR_DEF_TYPE, &def) != 0) {
        return 0;
    }

    uint64_t cap = 0;
    if (arch_rdmsr_safe(FB_IA32_MTRR_CAP, &cap) != 0) {
        /* No MTRR capability register: the default type is all we have. */
        *known = true;
        return def;
    }

    uint64_t vcnt = cap & 0xFFu;
    uint64_t best = def;
    int best_rank = fb_mtrr_type_rank(def);

    for (uint64_t i = 0; i < vcnt && i < 16u; i++) {
        uint64_t base = 0, mask = 0;
        if (arch_rdmsr_safe(FB_IA32_MTRR_BASE(i), &base) != 0) continue;
        if (arch_rdmsr_safe(FB_IA32_MTRR_MASK(i), &mask) != 0) continue;

        uint64_t base_addr = base & ~0xFFFu;     /* reserved low bits */
        uint64_t mask_addr = mask & ~0xFFFu;

        /* The range covers [base_addr, base_addr + ~mask_addr]. */
        uint64_t range_end = base_addr + (~mask_addr & 0x0000FFFFFFFFFFFFULL);
        if (pa + size <= base_addr || pa > range_end) {
            continue;   /* does not cover our range */
        }

        int rank = fb_mtrr_type_rank(base);
        if (rank < best_rank) {
            best_rank = rank;
            best = base & 0xFFu;
        }
    }

    *known = true;
    return best;
}

static void fb_x86_log_page_table_state(uint64_t first_leaf, uint64_t last_leaf,
                                        uint64_t pages, uint64_t pa)
{
    uint64_t pat = 0, cap = 0, def = 0, cr0 = 0;
    int pat_ok  = (arch_rdmsr_safe(FB_IA32_PAT, &pat) == 0);
    int cap_ok  = (arch_rdmsr_safe(FB_IA32_MTRR_CAP, &cap) == 0);
    int def_ok  = (arch_rdmsr_safe(FB_IA32_MTRR_DEF_TYPE, &def) == 0);

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    int mtrr_capable = (int)((edx >> 12) & 1u);

    __asm__ __volatile__("movq %%cr0, %0" : "=r"(cr0));

    bool mtrr_known = false;
    uint64_t mtrr_type = fb_x86_mtrr_type_for(pa, 0, &mtrr_known);

    log_info("fb: leaf_first=0x%lx leaf_last=0x%lx pages=%lu pa=0x%lx\n",
             first_leaf, last_leaf, pages, pa);
    log_info("fb: pat=0x%lx(%d) mtrr_capable=%d mtrr_cap=0x%lx(%d) mtrr_def=0x%lx(%d) "
             "mtrr_type_for_pa=%s(%d) cr0=0x%lx cd=%d nw=%d\n",
             pat, pat_ok, mtrr_capable, cap, cap_ok, def, def_ok,
             fb_mtrr_type_name(mtrr_type), mtrr_known,
             cr0, (int)((cr0 >> 30) & 1), (int)((cr0 >> 29) & 1));
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
        pmd[pmd_idx] = (pa & PAGE_2M_MASK) | flags;
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
        uint64_t expected = (pa & PAGE_2M_MASK) | pmd_flags;

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
                                 (uint64_t)num_pages, phys);
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
