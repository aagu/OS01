// kernel/arch/aarch64/driver/virtio_mmio.c — aarch64 virtio-mmio facade.
//
// Provides the platform slot enumeration (from DTB) and pre-SMP TTBR1
// Device-memory window mapping for the arch-neutral virtio-blk driver.

#include <arch/virtio_mmio.h>
#include <arch/aarch64/virtio_mmio.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>  // aarch64_read_ttbr1
#include <arch/aarch64/uefi_memory_map.h>
#include <arch/mmu.h>
#include <core/bootinfo.h>
#include <errno.h>

#define VIRTIO_MMIO_REG_SPAN  UINT64_C(0x200)
#define VIRTIO_MMIO_BLOCK_SZ  UINT64_C(0x200000)

static bool s_window_mapped;

static int map_one_block(uint64_t *root, const void *mmap_ptr,
                         uint32_t stride, uint32_t count, uint64_t blk_pa)
{
    int overlap = aarch64_uefi_range_overlaps_ram(
        mmap_ptr, stride, count, blk_pa, blk_pa + VIRTIO_MMIO_BLOCK_SZ);

    if (overlap != 0)
        return -EFAULT;
    return aarch64_pt_ensure_device_2m(root, ARCH_PAGE_OFFSET + blk_pa, blk_pa);
}

int aarch64_virtio_mmio_prepare(const struct boot_context *ctx)
{
    uint32_t slot_count;
    const void *mmap_ptr;
    uint32_t stride;
    uint32_t desc_count;
    uint64_t map_bytes;
    uint64_t map_end;
    uint64_t ttbr_raw;
    uint64_t root_pa;
    uint64_t *root;

    if (s_window_mapped)
        return 0;

    slot_count = dtb_virtio_mmio_count();
    if (slot_count == 0u) {
        s_window_mapped = true;
        return 0;
    }

    if (!boot_context_valid(ctx))
        return -EINVAL;
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0u ||
        ctx->memory.format != BOOT_MEMORY_FORMAT_UEFI_RAW ||
        ctx->memory.entries == 0u ||
        ctx->memory.entry_size < AARCH64_UEFI_MEM_DESC_MIN_SIZE ||
        ctx->memory.entry_count == 0u)
        return -EINVAL;

    stride = ctx->memory.entry_size;
    desc_count = ctx->memory.entry_count;
    if ((uint64_t)desc_count > UINT64_MAX / stride)
        return -EINVAL;
    map_bytes = (uint64_t)desc_count * stride;
    if (ctx->memory.entries > UINT64_MAX - map_bytes)
        return -EINVAL;
    map_end = ctx->memory.entries + map_bytes;
    if (map_end > (uint64_t)AARCH64_BOOT_MAP_END)
        return -EINVAL;
    if (ctx->memory.entries < (uint64_t)AARCH64_BOOT_BLOCK_END &&
        map_end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)
        return -EINVAL;

    mmap_ptr = (const void *)(uintptr_t)ctx->memory.entries;
    ttbr_raw = aarch64_read_ttbr1();
    root_pa = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    root = (uint64_t *)(uintptr_t)(root_pa + ARCH_PAGE_OFFSET);

    for (uint32_t i = 0u; i < slot_count; i++) {
        const struct aarch64_virtio_mmio_slot *slot = dtb_virtio_mmio_get(i);
        uint64_t first_blk;
        uint64_t last_blk;
        int rc;

        if (!slot || slot->base == 0u ||
            slot->base > UINT64_MAX - VIRTIO_MMIO_REG_SPAN)
            return -EINVAL;

        first_blk = slot->base & ~(VIRTIO_MMIO_BLOCK_SZ - 1u);
        last_blk = (slot->base + VIRTIO_MMIO_REG_SPAN - 1u) &
                   ~(VIRTIO_MMIO_BLOCK_SZ - 1u);

        rc = map_one_block(root, mmap_ptr, stride, desc_count, first_blk);
        if (rc != 0)
            return rc;
        if (last_blk != first_blk) {
            rc = map_one_block(root, mmap_ptr, stride, desc_count, last_blk);
            if (rc != 0)
                return rc;
        }
    }

    s_window_mapped = true;
    return 0;
}

int arch_virtio_mmio_map_early(void)
{
    return s_window_mapped ? 0 : -ENODEV;
}

uint32_t arch_virtio_mmio_slot_count(void)
{
    return dtb_virtio_mmio_count();
}

int arch_virtio_mmio_slot_get(uint32_t index,
                              struct arch_virtio_mmio_slot *out)
{
    if (!out)
        return -EINVAL;
    const struct aarch64_virtio_mmio_slot *slot = dtb_virtio_mmio_get(index);
    if (!slot)
        return -ENOENT;
    out->base = slot->base;
    out->irq  = slot->spi;
    return 0;
}
