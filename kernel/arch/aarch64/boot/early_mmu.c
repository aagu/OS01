#include <arch/aarch64/boot_layout.h>
#include <arch/aarch64/device_window.h>
#include <arch/aarch64/early_mmu.h>
#include <arch/aarch64/vmm_backend.h>

__attribute__((section(".boot.text.early_mmu")))
bool aarch64_boot_validate_handoff(const struct boot_context *ctx)
{
    uint64_t addr = (uint64_t)(uintptr_t)ctx;
    uint64_t end;

    if (addr == 0u || (addr & UINT64_C(7)) != 0u)
        return false;
    if (addr > (uint64_t)AARCH64_BOOT_MAP_END - sizeof(*ctx))
        return false;
    end = addr + sizeof(*ctx);
    if (addr < (uint64_t)AARCH64_BOOT_BLOCK_END &&
        end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)
        return false;
    return aarch64_early_mmio_valid(ctx);
}

#define BOOT_INNER_SHAREABLE (UINT64_C(3) << 8)

__attribute__((section(".boot.text.early_mmu")))
void aarch64_build_boot_page_tables(uint64_t tables[AARCH64_BOOT_PT_COUNT][512], uint64_t boot_base,
                                    uint64_t device_base, uint64_t device_end)
{
    const uint64_t table_flags = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
    const uint64_t block_flags = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_AF |
                                 AARCH64_PT_DESC_UXN;

    tables[0][0] = (uintptr_t)tables[1] | table_flags;
    for (unsigned l1 = 0; l1 < AARCH64_BOOT_MAP_L2_COUNT; ++l1)
        tables[1][l1] = (uintptr_t)tables[2 + l1] | table_flags;

    uint64_t pa = 0;
    for (unsigned table = 2; table < 2 + AARCH64_BOOT_MAP_L2_COUNT; ++table) {
        for (unsigned slot = 0; slot < 512; ++slot, pa += AARCH64_BOOT_BLOCK_SIZE) {
            uint64_t attrs = pa >= device_base && pa < device_end
                ? AARCH64_PT_ATTR_DEVICE
                : AARCH64_PT_ATTR_NORMAL | BOOT_INNER_SHAREABLE;
            uint64_t pxn = pa == boot_base ? 0 : AARCH64_PT_DESC_PXN;
            tables[table][slot] = pa | block_flags | attrs | pxn;
        }
    }
}

/* Device blocks are never executable and never shareable: the same
 * descriptor the builder above emits inside the low window. */
#define BOOT_DEVICE_BLOCK (AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_AF | \
                           AARCH64_PT_DESC_UXN | AARCH64_PT_DESC_PXN | \
                           AARCH64_PT_ATTR_DEVICE)
/* Table descriptors carry the next level's address in bits [47:12]. */
#define BOOT_TABLE_PA_MASK UINT64_C(0x0000ffffffff000)

__attribute__((section(".boot.text.early_mmu")))
void aarch64_build_boot_window(uint64_t tables[AARCH64_BOOT_PT_COUNT][512], unsigned table,
                               uint64_t base, uint64_t end)
{
    const uint64_t table_flags = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
    const uint64_t low_end = (uint64_t)AARCH64_BOOT_MAP_END;
    const uint64_t block = (uint64_t)AARCH64_BOOT_BLOCK_SIZE;

    if (base < low_end) {
        /* Inside the identity map: the builder left these blocks Normal. */
        uint64_t *pmd = tables[2 + (unsigned)(base >> 30)];
        for (uint64_t pa = base; pa < end; pa += block)
            pmd[(pa >> 21) & 511] = pa | BOOT_DEVICE_BLOCK;
        return;
    }
    {
        /* One L1 slot (1 GiB) per window; a slot may already carry the
         * other window's table, which is then extended in place. */
        uint64_t *pmd;
        uint64_t desc = tables[1][base >> 30];
        if (desc & AARCH64_PT_DESC_VALID) {
            pmd = (uint64_t *)(uintptr_t)(desc & BOOT_TABLE_PA_MASK);
        } else {
            pmd = tables[table];
            tables[1][base >> 30] = (uintptr_t)pmd | table_flags;
        }
        for (unsigned i = 0; i < 512; ++i) {
            uint64_t pa = (base & ~(uint64_t)0x3fffffff) + ((uint64_t)i << 21);
            if (pa >= base && pa < end)
                pmd[i] = pa | BOOT_DEVICE_BLOCK;
        }
    }
}
