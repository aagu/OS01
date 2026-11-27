/* kernel/arch/aarch64/bus/rp1_pcie.c — WoR BCM2712 PCIe RC & RP1 resource decoder */
#include <arch/aarch64/rp1_xhci.h>
#include <errno.h>
#include <string.h>

#define BCM2712_RC_BUS_NUM             0x0018U
#define BCM2712_RC_MEM_BASE_LIMIT      0x0020U
#define BCM2712_OUT_MEM_WIN0_LO        0x400cU
#define BCM2712_OUT_MEM_WIN0_HI        0x4010U
#define BCM2712_BAR2_CONFIG_LO         0x4034U
#define BCM2712_BAR2_CONFIG_HI         0x4038U
#define BCM2712_PCIE_STATUS            0x4068U
#define BCM2712_OUT_MEM_WIN0_BL        0x4070U
#define BCM2712_OUT_MEM_WIN0_BHI       0x4080U
#define BCM2712_OUT_MEM_WIN0_LHI       0x4084U
#define BCM2712_UBUS_BAR2_REMAP_LO     0x40acU
#define BCM2712_UBUS_BAR2_REMAP_HI     0x40b0U
#define BCM2712_EXT_CFG_DATA           0x8000U

#define BCM2712_LINK_UP_MASK           0x30U
#define RP1_VENDOR_ID                  0x1de4U
#define RP1_DEVICE_ID                  0x0001U

int aarch64_rp1_pcie_resources(const struct aarch64_rp1_xhci_desc *desc,
                               const struct aarch64_rp1_pcie_io *io,
                               struct aarch64_rp1_pcie_resources *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!desc || !desc->valid || !io || !io->rc_read32 ||
        !io->cfg_save_select || !io->cfg_restore) {
        return -EINVAL;
    }

    uint32_t status = io->rc_read32(io->ctx, BCM2712_PCIE_STATUS);
    if ((status & BCM2712_LINK_UP_MASK) != BCM2712_LINK_UP_MASK)
        return -ENODEV;

    uint32_t bus_num = io->rc_read32(io->ctx, BCM2712_RC_BUS_NUM);
    uint8_t sec_bus = (uint8_t)((bus_num >> 8) & 0xFFU);
    if (sec_bus == 0U)
        return -ENODEV;

    uint32_t mem_bl = io->rc_read32(io->ctx, BCM2712_RC_MEM_BASE_LIMIT);
    uint64_t bridge_base = (uint64_t)(mem_bl & 0xFFF0U) << 16;
    uint64_t bridge_limit = ((uint64_t)(mem_bl & 0xFFF00000U)) | 0xFFFFFULL;
    if (bridge_limit < bridge_base)
        return -EINVAL;

    uint64_t out_pci_base = ((uint64_t)io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_HI) << 32) |
                            (uint64_t)(io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_LO) & ~0xFULL);
    uint32_t out_bl = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_BL);
    uint32_t out_bhi = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_BHI) & 0xFFU;
    uint32_t out_lhi = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_LHI) & 0xFFU;
    uint64_t out_cpu_base = ((uint64_t)out_bhi << 32) | ((uint64_t)(out_bl & 0xFFF0U) << 16);
    uint64_t out_cpu_last = ((uint64_t)out_lhi << 32) | ((uint64_t)(out_bl & 0xFFF00000U)) | 0xFFFFFULL;
    if (out_cpu_last < out_cpu_base)
        return -EINVAL;
    uint64_t out_size = (out_cpu_last - out_cpu_base) + 1ULL;
    uint64_t out_cpu_end = out_cpu_base + out_size;

    uint32_t bdf = ((uint32_t)sec_bus << 20);
    uint32_t saved_sel = io->cfg_save_select(io->ctx, bdf);
    uint32_t ep_id = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x00U);
    uint32_t ep_cmd = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x04U);
    uint32_t bar1_lo = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x14U);
    uint32_t bar1_hi = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x18U);
    io->cfg_restore(io->ctx, saved_sel);

    if ((ep_id & 0xFFFFU) != RP1_VENDOR_ID || ((ep_id >> 16) & 0xFFFFU) != RP1_DEVICE_ID)
        return -ENODEV;
    if ((ep_cmd & 0x0006U) != 0x0006U) /* MEM + BME must be enabled by firmware */
        return -EIO;
    if ((bar1_lo & 0x1U) != 0U)
        return -EINVAL;

    uint32_t bar_type = (bar1_lo >> 1) & 0x3U;
    uint64_t bar1_pci = 0ULL;
    if (bar_type == 0U) {
        bar1_pci = (uint64_t)(bar1_lo & ~0xFULL);
    } else if (bar_type == 2U) {
        bar1_pci = ((uint64_t)bar1_hi << 32) | (uint64_t)(bar1_lo & ~0xFULL);
    } else {
        return -EINVAL;
    }

    /* Check BAR1 inside bridge window and outbound window. */
    uint64_t rp1_span = 0x400000ULL;
    if (bar1_pci < bridge_base || bar1_pci + rp1_span - 1ULL > bridge_limit)
        return -EFAULT;
    if (bar1_pci < out_pci_base || (bar1_pci - out_pci_base) + rp1_span > out_size)
        return -EFAULT;

    uint64_t bar1_cpu = out_cpu_base + (bar1_pci - out_pci_base);

    /* Validate RC BAR2 / UBUS identity inbound window. */
    uint32_t bar2_lo = io->rc_read32(io->ctx, BCM2712_BAR2_CONFIG_LO);
    uint32_t bar2_hi = io->rc_read32(io->ctx, BCM2712_BAR2_CONFIG_HI);
    uint32_t size_enc = bar2_lo & 0x1FU;
    if (size_enc == 0U || size_enc > 21U)
        return -EFAULT;
    uint64_t in_size = 1ULL << (size_enc + 15U);
    uint64_t in_pci = ((uint64_t)bar2_hi << 32) | (uint64_t)(bar2_lo & ~0x3FULL);

    uint32_t remap_lo = io->rc_read32(io->ctx, BCM2712_UBUS_BAR2_REMAP_LO);
    uint32_t remap_hi = io->rc_read32(io->ctx, BCM2712_UBUS_BAR2_REMAP_HI);
    if ((remap_lo & 0x1U) == 0U)
        return -EFAULT;
    uint64_t in_cpu = ((uint64_t)remap_hi << 32) | (uint64_t)(remap_lo & ~0xFFFULL);
    if (in_pci != 0ULL || in_cpu != 0ULL)
        return -EFAULT;

    uint64_t dma_limit = in_size;
    if (dma_limit > AARCH64_RP1_DMA_HARD_LIMIT)
        dma_limit = AARCH64_RP1_DMA_HARD_LIMIT;
    if (out_cpu_base < dma_limit && out_cpu_end > 0ULL)
        dma_limit = out_cpu_base;
    if (dma_limit == 0ULL)
        return -EFAULT;

    out->valid = true;
    out->bar1_cpu_base = bar1_cpu;
    out->bar1_pci_base = bar1_pci;
    out->dma_min_pa = 0ULL;
    out->dma_limit_pa = dma_limit;
    out->outbound_cpu_base = out_cpu_base;
    out->outbound_cpu_limit = out_cpu_end;
    out->secondary_bus = sec_bus;
    return 0;
}
