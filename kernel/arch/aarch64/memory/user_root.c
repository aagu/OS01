#include <arch/aarch64/page_table.h>
#include <arch/mmu.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/mmu.h>

bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                uint64_t len, bool writable)
{
    if (!len)
        return true;
    if (addr < AARCH64_USER_MIN || addr >= AARCH64_USER_LIMIT ||
        len > AARCH64_USER_LIMIT - addr)
        return false;
    return aarch64_pt_range_accessible(pgtbl, addr, len, writable, true);
}

#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/vmm_gate.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <stddef.h>

uint64_t *aarch64_el0_root_create(void)
{
    uint64_t pa = alloc_4k_page();
    if (!pa)
        return NULL;
    uint64_t *root = (uint64_t *)Phy_To_Virt(pa);
    __builtin_memset(root, 0, PAGE_4K_SIZE);
    return root;
}

int aarch64_el0_map(uint64_t *root, uint64_t va, uint64_t pa, uint32_t perm)
{
    if (!root || va < AARCH64_USER_MIN || va >= AARCH64_USER_LIMIT ||
        (perm != AARCH64_PT_USER_RO &&
         perm != (AARCH64_PT_USER_RO | AARCH64_PT_EXEC) &&
         perm != AARCH64_PT_USER_RW) ||
        !pmm_4k_page_allocated(pa))
        return AARCH64_PT_EINVAL;
    uint64_t root_pa = (uintptr_t)root - ARCH_PAGE_OFFSET;
    if (aarch64_pt_root_is_published(&root_pa))
        return AARCH64_PT_EPERM;
    return aarch64_pt_map_4k(root, va, pa, perm);
}

/* All intermediate descriptors were allocated by M3 for 4 KiB
 * mappings. Stop at L3: leaves belong to the caller. Empty tables
 * left by a partial map failure are reclaimed by the same walk. */
static void free_tables(uint64_t *table, unsigned level)
{
    if (level < 3) {
        for (unsigned i = 0; i < PAGE_4K_SIZE / sizeof(*table); i++) {
            uint64_t desc = table[i];
            const uint64_t bits = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
            if ((desc & bits) == bits)
                free_tables((uint64_t *)Phy_To_Virt(desc & AARCH64_TTBR_BASE_MASK),
                            level + 1);
        }
    }
    free_4k_page((uintptr_t)table - ARCH_PAGE_OFFSET);
}

void aarch64_el0_root_destroy(uint64_t *root)
{
    if (root) {
        uint64_t pa = (uintptr_t)root - ARCH_PAGE_OFFSET;
        if (aarch64_pt_root_is_published(&pa)) {
            vmm_gate_violation("destroying an active EL0 root");
            return;
        }
        free_tables(root, 0);
    }
}

#endif
