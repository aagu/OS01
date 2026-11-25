// kernel/arch/aarch64/driver/virtio_mmio.c — aarch64 virtio-mmio facade.
//
// Provides the platform slot enumeration (from DTB) and pre-SMP TTBR1
// Device-memory window mapping for the arch-neutral virtio-blk driver.

#include <arch/virtio_mmio.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>  // aarch64_read_ttbr1
#include <arch/mmu.h>
#include <errno.h>

static bool s_window_mapped;

int arch_virtio_mmio_map_early(void)
{
#if defined(AARCH64_VIRTIO_MMIO) && !AARCH64_VIRTIO_MMIO
    /* No virtio-mmio on this platform: nothing to map. */
    return 0;
#endif
    if (s_window_mapped)
        return 0;

    uint64_t pa   = 0x0a000000UL;
    uint64_t va   = ARCH_PAGE_OFFSET + pa;
    uint32_t perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE;

    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa  = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *root    = (uint64_t *)(uintptr_t)(root_pa + ARCH_PAGE_OFFSET);

    int rc = aarch64_pt_map_2m_block(root, va, pa, perm);
    if (rc == 0)
        s_window_mapped = true;
    return rc;
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
