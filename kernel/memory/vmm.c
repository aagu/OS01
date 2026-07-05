#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/debug.h>
#include <kernel/printk.h>
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
void vmm_map_page(uint64_t *pagemap, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags)
{
    uint64_t *pml4, *pml3, *pml2;
    size_t level4, level3, level2;

    level4 = (size_t) (virtual_address >> PAGE_GDT_SHIFT) & 0x1ff;
    level3 = (size_t) (virtual_address >> PAGE_1G_SHIFT) & 0x1ff;
    level2 = (size_t) (virtual_address >> PAGE_2M_SHIFT) & 0x1ff;

    pml4 = pagemap;
    // Use user-accessible intermediate levels when mapping a user page
    uint64_t gdt_flags = (flags & PAGE_U_S) ? PAGE_USER_GDT : PAGE_KERNEL_GDT;
    uint64_t dir_flags = (flags & PAGE_U_S) ? PAGE_USER_Dir : PAGE_KERNEL_Dir;
    pml3 = get_next_level(pml4, level4, gdt_flags);
    pml2 = get_next_level(pml3, level3, dir_flags);
    pml2[level2] = (physical_address & PAGE_2M_MASK) | flags;

    // If modifying the shared kernel page table after APs are online,
    // broadcast TLB invalidation so other cores drop stale PDE/TLB entries.
    if (pagemap == kernel_map && num_cpus > 1)
        tlb_shootdown();
}

// unmap virtual page to physical address, return the physical address
uintptr_t vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address)
{
    uint64_t *pml4, *pml3, *pml2;
    size_t level4, level3, level2;

    level4 = (size_t) (virtual_address >> PAGE_GDT_SHIFT) & 0x1ff;
    level3 = (size_t) (virtual_address >> PAGE_1G_SHIFT) & 0x1ff;
    level2 = (size_t) (virtual_address >> PAGE_2M_SHIFT) & 0x1ff;

    pml4 = pagemap;
    if (!(pml4[level4] & PAGE_Present))
        return 0;
    pml3 = (uint64_t *)Phy_To_Virt(pml4[level4] & PAGE_4K_MASK);
    if (!(pml3[level3] & PAGE_Present))
        return 0;
    pml2 = (uint64_t *)Phy_To_Virt(pml3[level3] & PAGE_4K_MASK);

    uintptr_t phys = pml2[level2] & (PAGE_2M_MASK & ~PAGE_XD);
    pml2[level2] = 0;
    return phys;
}

static void dump_memory_map()
{
    mem_dump(kernel_map, kernel_map+512);
    // mem_dump(kernel_map+PAGE_4K_SIZE, kernel_map+512+PAGE_4K_SIZE);
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
            vmm_map_page(kernel_map, p->phy_address, (uintptr_t)Phy_To_Virt(p->phy_address), PAGE_KERNEL_Page);
            #ifdef DEBUG
            if(j % 50 == 0)
            {
                uint64_t *pml4, *pml3, *pml2;
                size_t level4, level3, level2;

                level4 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_GDT_SHIFT) & 0x1ff;
                level3 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_1G_SHIFT) & 0x1ff;
                level2 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_2M_SHIFT) & 0x1ff;

                pml4 = kernel_map;
                pml3 = get_next_level(pml4, level4, PAGE_KERNEL_GDT);
                pml2 = get_next_level(pml3, level3, PAGE_KERNEL_Dir);

                // pml2[level2] = 0;
                debug_mm("-----\t\n");
                debug_mm("pml3:%#018lx,%#018lx\t\n",(unsigned long)pml3,pml3[level3]);
                debug_mm("pml2:%#018lx,%#018lx\t\n",(unsigned long)pml2,pml2[level2]);
            }
            #endif
        }
    }
    
    tlb_shootdown();
}

mmap vmm_alloc_map() {
    return (mmap)calloc(1, PAGE_4K_SIZE);
}

void vmm_free_user_map(mmap pagemap)
{
    if (!pagemap)
        return;

    // Walk PML4 entries 0–255 (user half). Entries 256–511 are
    // shared kernel entries and must not be freed.
    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t pml4e = pagemap[l4];
        if (!(pml4e & PAGE_Present))
            continue;

        uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);

        // Walk PDPT entries (pointers to PDE pages)
        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t pml3e = pml3[l3];
            if (!(pml3e & PAGE_Present))
                continue;

            uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);

            // Walk PDE entries (2MB pages via PAGE_PS)
            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pml2e = pml2[l2];
                if (!(pml2e & PAGE_Present))
                    continue;

                // Only 2MB pages (PAGE_PS set) are supported
                if (pml2e & PAGE_PS) {
                    uintptr_t phys = pml2e & (PAGE_2M_MASK & ~PAGE_XD);
                    struct Page *page = Phy_to_2M_Page(phys);
                    page_clean(page);
                    free_pages(page, 1);
                }
            }

            kfree(pml2);
        }

        kfree(pml3);
    }

    kfree(pagemap);
}

// Walk PML4→PML3→PML2→PTE, return pointer to PTE[level1] entry.
// If allocate=true, allocates missing intermediate tables via calloc.
// flags carries PAGE_U_S for user-accessible intermediate levels.
// Returns NULL if allocate fails (OOM) or a required table is missing
// with allocate=false.
uint64_t *vmm_pt_walk(uint64_t *pagemap, uint64_t virt,
                      uint64_t flags, int allocate)
{
    size_t l4 = (size_t)(virt >> PAGE_GDT_SHIFT) & 0x1ff;
    size_t l3 = (size_t)(virt >> PAGE_1G_SHIFT)  & 0x1ff;
    size_t l2 = (size_t)(virt >> PAGE_2M_SHIFT)  & 0x1ff;
    size_t l1 = (size_t)(virt >> PAGE_4K_SHIFT)  & 0x1ff;

    // User-half only: entries 0–255.  l4 >= 256 are kernel entries
    // shared via memcpy(&child_pml4[256], ...) — must never be touched.
    if (l4 >= 256) return NULL;

    uint64_t *pml4 = pagemap;
    uint64_t gdt_flags = (flags & PAGE_U_S) ? PAGE_USER_GDT : PAGE_KERNEL_GDT;
    uint64_t dir_flags = (flags & PAGE_U_S) ? PAGE_USER_Dir : PAGE_KERNEL_Dir;

    // PML4 → PML3
    if (!(pml4[l4] & PAGE_Present)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;  // OOM check before Virt_To_Phy(0)
        pml4[l4] = Virt_To_Phy((uint64_t)t) | gdt_flags;
    }
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);

    // PML3 → PML2 (PDE table)
    if (!(pml3[l3] & PAGE_Present)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;
        pml3[l3] = Virt_To_Phy((uint64_t)t) | dir_flags;
    }
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);

    // PML2 → PTE table (4KB leaf).
    // Guard: if PML2[l2] is a 2MB huge page (PAGE_PS), return NULL.
    // 4KB operations must not walk into a 2MB PDE as if it were a PTE table.
    if (pml2[l2] & PAGE_PS)
        return NULL;

    if (!(pml2[l2] & PAGE_Present)) {
        if (!allocate) return NULL;
        void *t = calloc(1, PAGE_4K_SIZE);
        if (!t) return NULL;
        pml2[l2] = Virt_To_Phy((uint64_t)t) | dir_flags;
    }
    uint64_t *pte_table = (uint64_t *)Phy_To_Virt(pml2[l2] & PAGE_4K_MASK);

    return &pte_table[l1];
}

// Map a 4KB physical page at virt.  Returns 0 on success, -ENOMEM if
// PTE table allocation fails.  Caller must free phys on failure.
int vmm_map_4k_page(uint64_t *pagemap, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    uint64_t *pte = vmm_pt_walk(pagemap, virt, flags, 1);
    if (!pte)
        return -ENOMEM;

    *pte = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    return 0;
}

// Unmap a 4KB page at virt.  Frees the physical page via free_4k_page.
// Safe to call on unmapped/never-faulted pages (no-op).
// PTE table reclamation is deferred (V1: pages freed, tables remain).
void vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt)
{
    uint64_t *pte = vmm_pt_walk(pagemap, virt, 0, 0);
    if (!pte)
        return;

    // Must check both Present and PROTNONE — PROTNONE pages have
    // Present=0 but valid phys that must be freed.
    if (!(*pte & (PAGE_Present | PAGE_PROTNONE)))
        return;

    uint64_t phys = *pte & PAGE_4K_MASK;
    *pte = 0;
    free_4k_page(phys);
}