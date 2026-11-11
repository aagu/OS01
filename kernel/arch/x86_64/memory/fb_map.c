/*
 * kernel/arch/x86_64/memory/fb_map.c
 *
 * x86_64 checked 2MiB framebuffer page mapping.
 * (Spec §2.1, Task 2)
 */

#include <arch/x86_64/fb_map.h>
#include <arch/x86_64/msr.h>       /* rdmsr (guest-side cache-type diagnostic) */
#include <driver/fb_cache.h>
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
/* ── Guest-side cache-type diagnostic ────────────────────────────────
 *
 * The page-table bits are only half of the story: the memory type the
 * CPU actually applies to the framebuffer pages also depends on
 * IA32_PAT, CR0.CD/NW and the MTRRs.  A host test can check the bit
 * ENCODING, but not the type the CPU derives from it -- so the guest
 * reports the registers it really uses, once, right after the mapping
 * is installed.  The values are printed so a run's log carries the
 * evidence (leaf entry / PAT / CR0).
 *
 * IA32_PAT = 0x277.  The MTRR registers are NOT read here: OS01 never
 * programs them, and a plain RDMSR of IA32_MTRR_DEF_TYPE raises #GP on
 * this host -- the MTRR state is therefore reported as "not verified".
 */
static void fb_x86_log_effective_cache_type(uint64_t leaf)
{
    uint64_t pat = rdmsr(0x277);          /* IA32_PAT */
    uint64_t cr0 = 0;

    __asm__ __volatile__("movq %%cr0, %0" : "=r"(cr0));

    log_info("fb: pmd_leaf=0x%lx pat=0x%lx cr0=0x%lx cd=%d nw=%d\n",
             leaf, pat, cr0,
             (int)((cr0 >> 30) & 1), (int)((cr0 >> 29) & 1));
}
#endif

int fb_x86_map_checked(uint64_t phys, uint64_t size, uint32_t **out_addr)
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
     * The policy is sampled ONCE here, so the install and the
     * verification below always agree on the expected entry value.
     * The host regression asserts the resulting page-table bits against
     * its own hard-coded expectation; it does not reuse this
     * expression. */
    fb_cache_policy_t policy = fb_cache_policy_get();
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
        if (g_mock_corrupt_pmd_phys) {
            pmd[pmd_idx] ^= 0x1000ULL;
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

    /* All entries verified: flush TLB / broadcast shootdown */
    tlb_shootdown();
    *out_addr = (uint32_t *)va_base;

#ifndef OS01_HOST_TEST
    fb_x86_log_effective_cache_type(records[0].pmd[records[0].pmd_idx]);
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
