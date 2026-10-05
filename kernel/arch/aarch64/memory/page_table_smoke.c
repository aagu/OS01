#include <errno.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/page_table_selftest.h>
#include <arch/aarch64/page_table.h>
#include <memory/memory.h>
#include <memory/pmm.h>
/* BSP owns this initially empty slot. No unrelated allocator/tree writer runs
 * during the smoke. Validate the whole partial subtree before detaching it. */
int aarch64_page_table_smoke_cleanup(uint64_t *root, uint64_t data_pa)
{
    uint64_t owned[3];
    size_t count = 0;
    const struct aarch64_early_arena *a = aarch64_early_arena_get();
    uint64_t d = root[256];
    for (size_t level = 0; level < 3 && d; level++) {
        if ((d & 3) != 3 || (d & ~UINT64_C(0x000000fffffff003)))
            return -EIO;
        uint64_t pa = d & UINT64_C(0x000000fffffff000);
        if (!pa || (pa >= a->base_pa && pa < a->end_pa) || pa == data_pa ||
            !pmm_4k_page_allocated(pa))
            return -EIO;
        for (size_t i = 0; i < count; i++)
            if (owned[i] == pa)
                return -EIO;
        owned[count++] = pa;
        const uint64_t *table = (const uint64_t *)Phy_To_Virt(pa);
        for (size_t i = 1; i < 512; i++)
            if (table[i])
                return -EIO;
        d = table[0];
        if (level == 2 && d && ((d & 3) != 3 || (d & UINT64_C(0x000000fffffff000)) != data_pa))
            return -EIO;
    }
    if (data_pa && !pmm_4k_page_allocated(data_pa))
        return -EIO;
    root[256] = 0;
    aarch64_tlb_flush_all();
    if (data_pa)
        free_4k_page(data_pa);
    for (size_t i = 0; i < count; i++)
        free_4k_page(owned[i]);
    return 0;
}
