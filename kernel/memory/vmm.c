#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/debug.h>
#include <kernel/printk.h>
#include <kernel/arch/mmu.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <driver/serial.h>

// kernel map
mmap kernel_map;

// get next Level of map
mmap get_next_level(uint64_t *current_level, size_t entry, uint64_t flags)
{
    if (!(current_level[entry] & 1))
    {
        current_level[entry] = Virt_To_Phy((uint64_t)calloc(1, PAGE_4K_SIZE));
        current_level[entry] |= flags;
    }
    return (uint64_t *)Phy_To_Virt((uint64_t)(current_level[entry] & PAGE_4K_MASK));
}

// map virtual page to physical address
void vmm_map_page(uint64_t *pgdir, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags)
{
    uint64_t *pgd, *pud, *pmd;
    size_t pgd_idx, pud_idx, pmd_idx;

    pgd_idx = (size_t) (virtual_address >> PAGE_PGD_SHIFT) & 0x1ff;
    pud_idx = (size_t) (virtual_address >> PAGE_1G_SHIFT)  & 0x1ff;
    pmd_idx = (size_t) (virtual_address >> PAGE_2M_SHIFT)  & 0x1ff;

    pgd = pgdir;
    // Use user-accessible intermediate levels when mapping a user page
    uint64_t pgd_flags = (flags & PAGE_USER) ? PAGE_USER_PGD : PAGE_KERNEL_PGD;
    uint64_t pud_flags = (flags & PAGE_USER) ? PAGE_USER_PUD : PAGE_KERNEL_PUD;
    pud = get_next_level(pgd, pgd_idx, pgd_flags);
    pmd = get_next_level(pud, pud_idx, pud_flags);
    pmd[pmd_idx] = (physical_address & PAGE_2M_MASK) | flags;

    // If modifying the shared kernel page table after APs are online,
    // broadcast TLB invalidation so other cores drop stale PMD/TLB entries.
    if (pgdir == kernel_map && num_cpus > 1)
        tlb_shootdown();
}

// unmap virtual page to physical address, return the physical address
uintptr_t vmm_unmap_page(uint64_t *pgdir, uintptr_t virtual_address)
{
    uint64_t *pgd, *pud, *pmd;
    size_t pgd_idx, pud_idx, pmd_idx;

    pgd_idx = (size_t) (virtual_address >> PAGE_PGD_SHIFT) & 0x1ff;
    pud_idx = (size_t) (virtual_address >> PAGE_1G_SHIFT)  & 0x1ff;
    pmd_idx = (size_t) (virtual_address >> PAGE_2M_SHIFT)  & 0x1ff;

    pgd = pgdir;
    if (!(pgd[pgd_idx] & PAGE_VALID))
        return 0;
    pud = (uint64_t *)Phy_To_Virt(pgd[pgd_idx] & PAGE_4K_MASK);
    if (!(pud[pud_idx] & PAGE_VALID))
        return 0;
    pmd = (uint64_t *)Phy_To_Virt(pud[pud_idx] & PAGE_4K_MASK);

    uintptr_t phys = pmd[pmd_idx] & (PAGE_2M_MASK & ~PAGE_NO_EXEC);
    pmd[pmd_idx] = 0;
    return phys;
}

void vmm_init()
{
    kernel_map = (uint64_t *)Phy_To_Virt(0x101000);
    uint64_t i, j;
    #ifdef DEBUG
    unsigned long * tmp = NULL;
    tmp = (unsigned long *)(((unsigned long)Phy_To_Virt((unsigned long)get_cr3() & (~ 0xfffUL))) + 8 * 256);

	debug_mm("1:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
	tmp = Phy_To_Virt(*tmp & (~0xfffUL));

	debug_mm("2:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
	tmp = Phy_To_Virt(*tmp & (~0xfffUL));
	debug_mm("3:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
    #endif

    for (i = 0; i < PMMngr.zones_size; i++)
    {
        struct Zone * z = PMMngr.zones_struct + i;
        struct Page * p = z->pages_group;

        if (ZONE_UNMAPPED_INDEX && i == ZONE_UNMAPPED_INDEX)
            break;

        for (j = 0; j < z->pages_length; j++, p++)
        {
            vmm_map_page(kernel_map, p->phy_address, (uintptr_t)Phy_To_Virt(p->phy_address), PAGE_KERNEL_PMD);
            #ifdef DEBUG
            if(j % 50 == 0)
            {
                uint64_t *pgd, *pud, *pmd;
                size_t pgd_idx, pud_idx, pmd_idx;

                pgd_idx = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_PGD_SHIFT) & 0x1ff;
                pud_idx = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_1G_SHIFT)  & 0x1ff;
                pmd_idx = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_2M_SHIFT)  & 0x1ff;

                pgd = kernel_map;
                pud = get_next_level(pgd, pgd_idx, PAGE_KERNEL_PGD);
                pmd = get_next_level(pud, pud_idx, PAGE_KERNEL_PUD);

                // pmd[pmd_idx] = 0;
                debug_mm("-----\t\n");
                debug_mm("pud:%#018lx,%#018lx\t\n",(unsigned long)pud,pud[pud_idx]);
                debug_mm("pmd:%#018lx,%#018lx\t\n",(unsigned long)pmd,pmd[pmd_idx]);
            }
            #endif
        }
    }

    tlb_shootdown();
}

mmap vmm_alloc_map() {
    return (mmap)calloc(1, PAGE_4K_SIZE);
}

void vmm_free_user_map(mmap pgdir)
{
    if (!pgdir)
        return;

    // Walk PGD entries 0-255 (user half). Entries 256-511 are
    // shared kernel entries and must not be freed.
    for (int l0 = 0; l0 < 256; l0++) {
        uint64_t pgde = pgdir[l0];
        if (!(pgde & PAGE_VALID))
            continue;

        uint64_t *pud = (uint64_t *)Phy_To_Virt(pgde & PAGE_4K_MASK);

        // Walk PUD entries (pointers to PMD pages)
        for (int l1 = 0; l1 < 512; l1++) {
            uint64_t pude = pud[l1];
            if (!(pude & PAGE_VALID))
                continue;

            uint64_t *pmd = (uint64_t *)Phy_To_Virt(pude & PAGE_4K_MASK);

            // Walk PMD entries (2MB pages via PAGE_HUGE)
            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pmde = pmd[l2];
                if (!(pmde & PAGE_VALID))
                    continue;

                if (pmde & PAGE_HUGE) {
                    uintptr_t phys = pmde & (PAGE_2M_MASK & ~PAGE_NO_EXEC);
                    struct Page *page = Phy_to_2M_Page(phys);
                    page_clean(page);
                    free_pages(page, 1);
                } else {
                    uint64_t *pt = (uint64_t *)Phy_To_Virt(pmde & PAGE_4K_MASK);
                    for (int l3 = 0; l3 < 512; l3++) {
                        uint64_t pte = pt[l3];
                        if (!(pte & (PAGE_VALID | PAGE_PROTNONE)))
                            continue;
                        uintptr_t phys = pte & PAGE_4K_MASK;
                        if (pte & PAGE_COW) {
                            if (page_cow_put(phys))
                                free_4k_page(phys);
                        } else {
                            free_4k_page(phys);
                        }
                    }
                    kfree(pt);
                }
            }

            kfree(pmd);
        }

        kfree(pud);
    }

    kfree(pgdir);
}

// Walk PGD->PUD->PMD->PTE, return pointer to PTE[level3] entry.
// If allocate=true, allocates missing intermediate tables via calloc.
// flags carries PAGE_USER for user-accessible intermediate levels.
// Returns NULL if allocate fails (OOM) or a required table is missing
// with allocate=false.
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    size_t l0 = (size_t)(virt >> PAGE_PGD_SHIFT) & 0x1ff;
    size_t l1 = (size_t)(virt >> PAGE_1G_SHIFT)  & 0x1ff;
    size_t l2 = (size_t)(virt >> PAGE_2M_SHIFT)  & 0x1ff;
    size_t l3 = (size_t)(virt >> PAGE_4K_SHIFT)  & 0x1ff;

    // User-half only: entries 0-255.  l0 >= 256 are kernel entries
    // shared via memcpy(&child_pgd[256], ...) -- must never be touched.
    if (l0 >= 256) return NULL;

    uint64_t *pgd = pgdir;
    uint64_t pgd_flags = (flags & PAGE_USER) ? PAGE_USER_PGD : PAGE_KERNEL_PGD;
    uint64_t pud_flags = (flags & PAGE_USER) ? PAGE_USER_PUD : PAGE_KERNEL_PUD;

    // PGD -> PUD
    if (!(pgd[l0] & PAGE_VALID)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;  // OOM check before Virt_To_Phy(0)
        pgd[l0] = Virt_To_Phy((uint64_t)t) | pgd_flags;
    }
    uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l0] & PAGE_4K_MASK);

    // PUD -> PMD
    if (!(pud[l1] & PAGE_VALID)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;
        pud[l1] = Virt_To_Phy((uint64_t)t) | pud_flags;
    }
    uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l1] & PAGE_4K_MASK);

    // PMD -> PTE table (4KB leaf).
    // Guard: if PMD[l2] is a 2MB huge page (PAGE_HUGE), return NULL.
    // 4KB operations must not walk into a 2MB PMD as if it were a PTE table.
    if (pmd[l2] & PAGE_HUGE)
        return NULL;

    if (!(pmd[l2] & PAGE_VALID)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;
        pmd[l2] = Virt_To_Phy((uint64_t)t) | pud_flags;
    }
    uint64_t *pte_table = (uint64_t *)Phy_To_Virt(pmd[l2] & PAGE_4K_MASK);

    return &pte_table[l3];
}

// Map a 4KB physical page at virt.  Returns 0 on success, -ENOMEM if
// PTE table allocation fails.  Caller must free phys on failure.
int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    uint64_t *pte = vmm_pt_walk(pgdir, virt, flags, 1);
    if (!pte)
        return -ENOMEM;

    *pte = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    return 0;
}

// Unmap a 4KB page at virt.  Frees the physical page via free_4k_page.
// Safe to call on unmapped/never-faulted pages (no-op).
// PTE table reclamation is deferred (V1: pages freed, tables remain).
void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    uint64_t *pte = vmm_pt_walk(pgdir, virt, 0, 0);
    if (!pte)
        return;

    // Must check both Valid and PROTNONE -- PROTNONE pages have
    // Valid=0 but valid phys that must be freed.
    if (!(*pte & (PAGE_VALID | PAGE_PROTNONE)))
        return;

    uint64_t phys = *pte & PAGE_4K_MASK;

    if (*pte & PAGE_COW) {
        // COW-shared page: decrement refcount, free only when count hits 0
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    *pte = 0;
}
