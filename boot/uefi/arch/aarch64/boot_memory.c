#include "loader.h"
#include "../../../../kernel/include/core/bootinfo.h"
#include "../../../../kernel/include/arch/aarch64/uefi_memory_map.h"

#define AARCH64_RPI5_FW_DTB_BASE  UINT64_C(0x1f0000)
#define AARCH64_RPI5_FW_DTB_END   UINT64_C(0x210000)
#define AARCH64_MMIO_BLOCK_SHIFT  21u
#define AARCH64_MMIO_BLOCK_BYTES  (UINT64_C(1) << AARCH64_MMIO_BLOCK_SHIFT)
#define AARCH64_MMIO_MAX_BLOCKS   1024u
#define AARCH64_L1_SLOT_SHIFT     30u
#define AARCH64_MAX_PA_LIMIT      (UINT64_C(1) << 39) /* 512 GiB */
#define AARCH64_PL011_SPAN        UINT64_C(0x48)

static bool intervals_overlap(uint64_t a0, uint64_t a1,
                              uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

static bool range_shape_valid(uint64_t base, uint64_t end)
{
    if (base >= end || end > AARCH64_MAX_PA_LIMIT)
        return false;
    if (base < AARCH64_BOOT_MAP_END && end > AARCH64_BOOT_MAP_END)
        return false;
    if (base >= AARCH64_BOOT_MAP_END &&
        (base >> AARCH64_L1_SLOT_SHIFT) != ((end - 1u) >> AARCH64_L1_SLOT_SHIFT))
        return false;
    if (intervals_overlap(base, end,
                          AARCH64_BOOT_BLOCK_BASE, AARCH64_BOOT_BLOCK_END))
        return false;
    return true;
}

bool aarch64_fw_dtb_readable(const void *map, uint32_t entry_size,
                             uint32_t count, uint64_t pa, uint64_t length)
{
    uint64_t end;
    uint64_t covered;

    if (!map || entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        count == 0u || length == 0u)
        return false;
    if ((uint64_t)count > UINT64_MAX / entry_size)
        return false;
    if (pa < AARCH64_RPI5_FW_DTB_BASE ||
        length > AARCH64_RPI5_FW_DTB_END - pa)
        return false;
    end = pa + length;

    /* Validate all descriptors first so corrupt maps never pass. */
    for (uint32_t i = 0u; i < count; i++) {
        struct aarch64_uefi_mem_desc desc;
        uint64_t desc_end = 0u;

        if (!aarch64_uefi_decode_desc(map, entry_size, i, &desc, &desc_end))
            return false;
    }

    covered = pa;
    for (uint32_t pass = 0u; pass < count && covered < end; pass++) {
        uint64_t next_covered = covered;

        for (uint32_t i = 0u; i < count; i++) {
            struct aarch64_uefi_mem_desc desc;
            uint64_t desc_end = 0u;

            if (!aarch64_uefi_decode_desc(map, entry_size, i, &desc, &desc_end))
                return false;
            if (desc.num_pages == 0u)
                continue;
            if (desc.type != AARCH64_UEFI_MEM_RESERVED &&
                desc.type != AARCH64_UEFI_MEM_CONVENTIONAL)
                continue;
            if ((desc.attribute & AARCH64_UEFI_ATTR_WB) == 0u ||
                (desc.attribute & AARCH64_UEFI_ATTR_RP) != 0u)
                continue;
            if (desc.phys_start <= covered && desc_end > next_covered)
                next_covered = desc_end;
        }
        if (next_covered == covered)
            break;
        covered = next_covered;
    }

    return covered >= end;
}

bool aarch64_early_mmio_map_safe(const struct boot_context *ctx)
{
    const void *map;
    uint32_t stride;
    uint32_t count;
    uint64_t win_base;
    uint64_t win_size;
    uint64_t win_end;
    uint64_t console_base;
    bool fw_console;

    if (!boot_context_valid(ctx))
        return false;
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0u ||
        ctx->memory.format != BOOT_MEMORY_FORMAT_UEFI_RAW ||
        ctx->memory.entries == 0u ||
        ctx->memory.entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        ctx->memory.entry_count == 0u)
        return false;
    if ((ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u)
        return false;

    map = (const void *)(uintptr_t)ctx->memory.entries;
    stride = ctx->memory.entry_size;
    count = ctx->memory.entry_count;

    win_base = ctx->early_mmio.device_base;
    if ((win_base & (AARCH64_MMIO_BLOCK_BYTES - 1u)) != 0u)
        return false;
    if (ctx->early_mmio.device_blocks == 0u ||
        ctx->early_mmio.device_blocks > AARCH64_MMIO_MAX_BLOCKS)
        return false;
    win_size = (uint64_t)ctx->early_mmio.device_blocks << AARCH64_MMIO_BLOCK_SHIFT;
    if (win_base > UINT64_MAX - win_size)
        return false;
    win_end = win_base + win_size;

    if (!range_shape_valid(win_base, win_end))
        return false;
    if (aarch64_uefi_range_overlaps_ram(map, stride, count, win_base, win_end) != 0)
        return false;

    console_base = ctx->early_mmio.console_base;
    if (console_base == 0u || (console_base & 3u) != 0u)
        return false;
    if (console_base > UINT64_MAX - AARCH64_PL011_SPAN ||
        console_base + AARCH64_PL011_SPAN > AARCH64_MAX_PA_LIMIT)
        return false;

    fw_console = (ctx->flags & BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED) != 0u;
    if (console_base >= win_base &&
        console_base + AARCH64_PL011_SPAN <= win_end)
        return true;

    if (!fw_console)
        return false;

    {
        uint64_t cblock_base = console_base & ~(AARCH64_MMIO_BLOCK_BYTES - 1u);
        uint64_t cblock_end = cblock_base + AARCH64_MMIO_BLOCK_BYTES;

        if (AARCH64_PL011_SPAN > cblock_end - console_base)
            return false;
        if (!range_shape_valid(cblock_base, cblock_end))
            return false;
        if (aarch64_uefi_range_overlaps_ram(map, stride, count,
                                            cblock_base, cblock_end) != 0)
            return false;
    }

    return true;
}
