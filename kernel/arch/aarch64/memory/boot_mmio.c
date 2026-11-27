/* kernel/arch/aarch64/memory/boot_mmio.c — M0 UEFI RAM/BootBlock validation & 2 MiB Device mapping */
#include <arch/aarch64/boot_mmio.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/boot_layout.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/uefi_memory_map.h>
#include <arch/mmu.h>
#include <memory/pmm.h>
#include <errno.h>
#include <log/log.h>
#include <stddef.h>

int aarch64_boot_mmio_ctx_init(const struct boot_context *boot,
                               struct aarch64_boot_mmio_ctx *out)
{
    if (!out)
        return -EINVAL;
    *out = (struct aarch64_boot_mmio_ctx){0};

    if (!boot_context_valid(boot) ||
        (boot->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0U ||
        boot->memory.format != BOOT_MEMORY_FORMAT_UEFI_RAW ||
        boot->memory.entries == 0U ||
        boot->memory.entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        boot->memory.entry_count == 0U) {
        return -EINVAL;
    }

    uint32_t stride = boot->memory.entry_size;
    uint32_t desc_count = boot->memory.entry_count;
    if ((uint64_t)desc_count > UINT64_MAX / stride)
        return -EINVAL;
    uint64_t map_bytes = (uint64_t)desc_count * stride;
    if (boot->memory.entries > UINT64_MAX - map_bytes)
        return -EINVAL;
    uint64_t uefi_map_end = boot->memory.entries + map_bytes;
    if (uefi_map_end > (uint64_t)AARCH64_BOOT_MAP_END ||
        (boot->memory.entries < (uint64_t)AARCH64_BOOT_BLOCK_END &&
         uefi_map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)) {
        return -EINVAL;
    }

    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *ttbr1_root = (uint64_t *)(uintptr_t)(ARCH_PAGE_OFFSET + root_pa);
    if (!ttbr1_root)
        return -EIO;

    out->mmap_ptr = (const void *)(uintptr_t)boot->memory.entries;
    out->stride = stride;
    out->desc_count = desc_count;
    out->ttbr1_root = ttbr1_root;
    return 0;
}

int aarch64_boot_mmio_map(const struct aarch64_boot_mmio_ctx *ctx,
                          uint64_t base, uint64_t size)
{
    if (!ctx || !ctx->mmap_ptr || !ctx->ttbr1_root || size == 0ULL)
        return -EINVAL;
    if (base > UINT64_MAX - size)
        return -EINVAL;

    const uint64_t blk_sz = (uint64_t)PAGE_2M_SIZE;
    const uint64_t blk_mask = (uint64_t)PAGE_2M_MASK;
    uint64_t map_start = base & blk_mask;
    uint64_t last_blk = (base + size - 1ULL) & blk_mask;
    if (last_blk > UINT64_MAX - blk_sz)
        return -EINVAL;
    uint64_t map_end = last_blk + blk_sz;
    if (map_start >= map_end || map_end > AARCH64_PT_PA_LIMIT)
        return -EINVAL;
    if (map_start < (uint64_t)AARCH64_BOOT_BLOCK_END &&
        map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE) {
        return -EFAULT;
    }

    for (uint64_t blk_pa = map_start; blk_pa < map_end; blk_pa += blk_sz) {
        int ov = aarch64_uefi_range_overlaps_ram(
            ctx->mmap_ptr, ctx->stride, ctx->desc_count, blk_pa, blk_pa + blk_sz);
        if (ov != 0)
            return -EFAULT;
        int mrc = aarch64_pt_ensure_device_2m(ctx->ttbr1_root, ARCH_PAGE_OFFSET + blk_pa, blk_pa);
        if (mrc != AARCH64_PT_OK)
            return -ENOMEM;
    }
    return 0;
}
