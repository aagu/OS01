#include <arch/aarch64/boot_layout.h>
#include <arch/aarch64/early_mmu.h>
#include <arch/aarch64/vmm_backend.h>

#define BOOT_INNER_SHAREABLE (UINT64_C(3) << 8)

__attribute__((section(".boot.text.early_mmu")))
void aarch64_build_boot_page_tables(uint64_t tables[6][512], uint64_t boot_base)
{
    const uint64_t table_flags = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_TABLE;
    const uint64_t block_flags = AARCH64_PT_DESC_VALID | AARCH64_PT_DESC_AF |
                                 AARCH64_PT_DESC_UXN;

    tables[0][0] = (uintptr_t)tables[1] | table_flags;
    tables[1][0] = (uintptr_t)tables[2] | table_flags;
    tables[1][1] = (uintptr_t)tables[3] | table_flags;

    uint64_t pa = 0;
    for (unsigned table = 2; table < 4; ++table) {
        for (unsigned slot = 0; slot < 512; ++slot, pa += AARCH64_BOOT_BLOCK_SIZE) {
            uint64_t attrs = pa >= AARCH64_BOOT_DEVICE_BASE &&
                             pa < AARCH64_BOOT_DEVICE_END
                ? AARCH64_PT_ATTR_DEVICE
                : AARCH64_PT_ATTR_NORMAL | BOOT_INNER_SHAREABLE;
            uint64_t pxn = pa == boot_base ? 0 : AARCH64_PT_DESC_PXN;
            tables[table][slot] = pa | block_flags | attrs | pxn;
        }
    }
}
