/*
 * kernel/arch/x86_64/memory/fb_map.c
 *
 * x86_64 checked 2MiB framebuffer page mapping.
 * (Spec §2.1, Task 2)
 */

#include <arch/x86_64/fb_map.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>
#include <core/printk.h>
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

        uint64_t flags = PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;
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
        uint64_t flags = PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;
        uint64_t expected = (pa & PAGE_2M_MASK) | flags;

        uint64_t actual = records[i].pmd[records[i].pmd_idx];
        if (actual != expected) {
            rc = -EIO;
            goto rollback;
        }
    }

    /* All entries verified: flush TLB / broadcast shootdown */
    tlb_shootdown();
    *out_addr = (uint32_t *)va_base;
    return 0;

rollback:
    /* Restore any written PMD entries to their previous values */
    for (size_t j = 0; j <= i && j < num_pages; j++) {
        if (records[j].written && records[j].pmd) {
            records[j].pmd[records[j].pmd_idx] = records[j].old_val;
        }
    }
    tlb_shootdown();
    *out_addr = NULL;
    return rc;
}
