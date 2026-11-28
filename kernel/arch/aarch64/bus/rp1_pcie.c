/* kernel/arch/aarch64/bus/rp1_pcie.c — WoR BCM2712 PCIe RC & RP1 resource decoder */
#include <arch/aarch64/rp1_xhci.h>
#include <arch/aarch64/rp1_pcie.h>
#include <errno.h>
#include <log/log.h>
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

int aarch64_rp1_pcie_endpoint(const struct aarch64_rp1_irq_desc *desc,
                              const struct aarch64_rp1_pcie_io *io,
                              struct aarch64_rp1_endpoint *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!desc || !desc->valid || !io || !io->rc_read32 ||
        !io->cfg_save_select || !io->cfg_restore)
        return -EINVAL;

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

    uint64_t out_pci_base =
        ((uint64_t)io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_HI) << 32) |
        (uint64_t)(io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_LO) & ~0xFULL);
    uint32_t out_bl = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_BL);
    uint32_t out_bhi = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_BHI) & 0xFFU;
    uint32_t out_lhi = io->rc_read32(io->ctx, BCM2712_OUT_MEM_WIN0_LHI) & 0xFFU;
    uint64_t out_cpu_base =
        ((uint64_t)out_bhi << 32) | ((uint64_t)(out_bl & 0xFFF0U) << 16);
    uint64_t out_cpu_last =
        ((uint64_t)out_lhi << 32) | ((uint64_t)(out_bl & 0xFFF00000U)) | 0xFFFFFULL;
    if (out_cpu_last < out_cpu_base)
        return -EINVAL;
    uint64_t out_size = (out_cpu_last - out_cpu_base) + 1ULL;

    uint32_t bdf = ((uint32_t)sec_bus << 20);
    uint32_t saved_sel = io->cfg_save_select(io->ctx, bdf);
    uint32_t ep_id = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x00U);
    uint32_t ep_cmd = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x04U);
    uint32_t ep_rev = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x08U);
    uint32_t bar0_lo = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x10U);
    uint32_t bar0_hi = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x14U);
    uint32_t bar1_lo = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x18U);
    uint32_t bar1_hi = io->rc_read32(io->ctx, BCM2712_EXT_CFG_DATA + 0x1cU);
    io->cfg_restore(io->ctx, saved_sel);

    if ((ep_id & 0xFFFFU) != RP1_VENDOR_ID ||
        ((ep_id >> 16) & 0xFFFFU) != RP1_DEVICE_ID)
        return -ENODEV;
    if ((ep_rev & 0xFFU) != 2U)
        return -ENODEV;
    if ((ep_cmd & 0x0006U) != 0x0006U)
        return -EIO;
    if ((bar0_lo & 0x1U) != 0U || (bar1_lo & 0x1U) != 0U)
        return -EINVAL;

    uint64_t bar0_pci = 0ULL, bar1_pci = 0ULL;
    uint32_t b0t = (bar0_lo >> 1) & 0x3U;
    uint32_t b1t = (bar1_lo >> 1) & 0x3U;
    if (b0t == 0U)
        bar0_pci = (uint64_t)(bar0_lo & ~0xFULL);
    else if (b0t == 2U)
        bar0_pci = ((uint64_t)bar0_hi << 32) | (uint64_t)(bar0_lo & ~0xFULL);
    else
        return -EINVAL;

    if (b1t == 0U)
        bar1_pci = (uint64_t)(bar1_lo & ~0xFULL);
    else if (b1t == 2U)
        bar1_pci = ((uint64_t)bar1_hi << 32) | (uint64_t)(bar1_lo & ~0xFULL);
    else
        return -EINVAL;

    uint64_t rp1_span = 0x400000ULL;
    if (bar0_pci < bridge_base || bar0_pci > bridge_limit ||
        bar0_pci < out_pci_base || (bar0_pci - out_pci_base) >= out_size)
        return -EFAULT;
    if (bar1_pci < bridge_base || bar1_pci + rp1_span - 1ULL > bridge_limit ||
        bar1_pci < out_pci_base || (bar1_pci - out_pci_base) + rp1_span > out_size)
        return -EFAULT;

    out->bus = sec_bus;
    out->device = 0U;
    out->function = 0U;
    out->revision = (uint8_t)(ep_rev & 0xFFU);
    out->bar0_pci = bar0_pci;
    out->bar0_cpu = out_cpu_base + (bar0_pci - out_pci_base);
    out->bar1_pci = bar1_pci;
    out->bar1_cpu = out_cpu_base + (bar1_pci - out_pci_base);
    out->outbound_pci_base = out_pci_base;
    out->outbound_cpu_base = out_cpu_base;
    out->outbound_size = out_size;
    return 0;
}

/* ── BCM2712 RC inbound MSI doorbell window (BAR4..10) ───────────────── */
#define BCM2712_BAR1_CFG_BASE    0x402cU
#define BCM2712_BAR1_REMAP_LO    0x40acU
#define BCM2712_BAR1_REMAP_HI    0x40b0U
#define BCM2712_INTERNAL_MSI_LO  0x4044U
#define BCM2712_INTERNAL_MSI_HI  0x4048U
#define BCM2712_BAR4_CFG_BASE    0x40d4U
#define BCM2712_BAR4_REMAP_BASE  0x410cU
#define BCM2712_EXTRA_WIN_COUNT  7U
#define BCM2712_SIZE_4K          0x1cU
#define BCM2712_WIN_4K_SPAN      0x1000ULL

static bool s_extra_win_quarantined[BCM2712_EXTRA_WIN_COUNT];

static int decode_rc_bar_size(uint32_t size_enc, uint64_t *span_out)
{
    if (size_enc == 0U) {
        *span_out = 0ULL;
        return 0;
    }
    if (size_enc >= 1U && size_enc <= 21U) {
        *span_out = 1ULL << (size_enc + 15U);
        return 0;
    }
    if (size_enc >= 0x1cU && size_enc <= 0x1fU) {
        *span_out = 1ULL << (size_enc - 0x1cU + 12U);
        return 0;
    }
    return -EFAULT;
}

static bool ranges_overlap_u64(uint64_t a0, uint64_t asz, uint64_t b0, uint64_t bsz)
{
    if (asz == 0ULL || bsz == 0ULL)
        return false;
    uint64_t a1 = (a0 > UINT64_MAX - asz) ? UINT64_MAX : (a0 + asz);
    uint64_t b1 = (b0 > UINT64_MAX - bsz) ? UINT64_MAX : (b0 + bsz);
    return a0 < b1 && b0 < a1;
}

int aarch64_rp1_msi_window_claim(const struct aarch64_rp1_irq_platform *p,
                                 struct aarch64_rp1_msi_window *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!p || !p->valid || !p->desc.valid)
        return -EINVAL;

    uint64_t target_pci = p->desc.msi_pci_addr & ~0xfffULL;
    uint64_t target_cpu = p->desc.mip_cpu_pa & ~0xfffULL;

    /* 1. Check RC internal MSI target trap (0x4044 bit 0). */
    uint32_t imsi_lo = 0U, imsi_hi = 0U;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_INTERNAL_MSI_LO, &imsi_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_INTERNAL_MSI_HI, &imsi_hi) != 0)
        return -EIO;
    if ((imsi_lo & 1U) != 0U) {
        uint64_t imsi_pci = ((uint64_t)imsi_hi << 32) | (uint64_t)(imsi_lo & ~0xfffU);
        if (ranges_overlap_u64(imsi_pci, BCM2712_WIN_4K_SPAN, target_pci, BCM2712_WIN_4K_SPAN))
            return -EEXIST;
    }

    /* 2. Check RC_BAR1..3 (read-only). */
    for (uint32_t k = 0U; k < 3U; k++) {
        uint32_t lo = 0U, hi = 0U;
        uint64_t span = 0ULL;
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR1_CFG_BASE + 8U * k, &lo) != 0 ||
            aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR1_CFG_BASE + 8U * k + 4U, &hi) != 0)
            return -EIO;
        uint32_t senc = lo & 0x1fU;
        if (decode_rc_bar_size(senc, &span) != 0)
            return -EFAULT;
        if (span == 0ULL)
            continue;
        uint64_t pci_base = ((uint64_t)hi << 32) | (uint64_t)(lo & ~0xfffU);
        if (k == 0U && senc == BCM2712_SIZE_4K && pci_base == target_pci) {
            uint32_t rlo = 0U, rhi = 0U;
            if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR1_REMAP_LO, &rlo) != 0 ||
                aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR1_REMAP_HI, &rhi) != 0)
                return -EIO;
            uint64_t cpu_base = ((uint64_t)rhi << 32) | (uint64_t)(rlo & ~0xfffU);
            if ((rlo & 1U) != 0U && cpu_base == target_cpu) {
                out->slot = 1U;
                out->valid = true;
                out->programmed = false;
                out->pci_addr = target_pci;
                out->cpu_addr = target_cpu;
                out->span = BCM2712_WIN_4K_SPAN;
                log_info("[rp1-irq-window] action=borrow slot=1 pci=0x%lx cpu=0x%lx span=0x%lx readback=ok\n",
                         (unsigned long)target_pci, (unsigned long)target_cpu,
                         (unsigned long)BCM2712_WIN_4K_SPAN);
                return 0;
            }
            return -EEXIST;
        }
        if (ranges_overlap_u64(pci_base, span, target_pci, BCM2712_WIN_4K_SPAN))
            return -EEXIST;
    }

    /* 3. Scan RC_BAR4..10 for exact borrow or first free slot. */
    int free_idx = -1;
    for (uint32_t i = 0U; i < BCM2712_EXTRA_WIN_COUNT; i++) {
        uint32_t cfg_lo = 0U, cfg_hi = 0U, rmp_lo = 0U, rmp_hi = 0U;
        uint64_t span = 0ULL;
        if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR4_CFG_BASE + 8U * i, &cfg_lo) != 0 ||
            aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR4_CFG_BASE + 8U * i + 4U, &cfg_hi) != 0 ||
            aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR4_REMAP_BASE + 8U * i, &rmp_lo) != 0 ||
            aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, BCM2712_BAR4_REMAP_BASE + 8U * i + 4U, &rmp_hi) != 0)
            return -EIO;

        uint32_t senc = cfg_lo & 0x1fU;
        bool rmp_en = (rmp_lo & 1U) != 0U;
        if (decode_rc_bar_size(senc, &span) != 0)
            return -EFAULT;

        if (senc == 0U && !rmp_en) {
            if (free_idx < 0 && !s_extra_win_quarantined[i])
                free_idx = (int)i;
            continue;
        }

        uint64_t pci_base = ((uint64_t)cfg_hi << 32) | (uint64_t)(cfg_lo & ~0xfffU);
        uint64_t cpu_base = ((uint64_t)rmp_hi << 32) | (uint64_t)(rmp_lo & ~0xfffU);
        if (senc == BCM2712_SIZE_4K && rmp_en &&
            pci_base == target_pci && cpu_base == target_cpu) {
            out->slot = (uint8_t)(4U + i);
            out->valid = true;
            out->programmed = false;
            out->pci_addr = target_pci;
            out->cpu_addr = target_cpu;
            out->span = BCM2712_WIN_4K_SPAN;
            log_info("[rp1-irq-window] action=borrow slot=%u pci=0x%lx cpu=0x%lx span=0x%lx readback=ok\n",
                     (unsigned)out->slot, (unsigned long)target_pci,
                     (unsigned long)target_cpu, (unsigned long)BCM2712_WIN_4K_SPAN);
            return 0;
        }
        if (senc != 0U && ranges_overlap_u64(pci_base, span, target_pci, BCM2712_WIN_4K_SPAN))
            return -EEXIST;
        if (senc == 0U && rmp_en && pci_base == target_pci)
            return -EEXIST;
    }

    if (free_idx < 0)
        return -ENOSPC;

    uint32_t idx = (uint32_t)free_idx;
    uint32_t off_cfg_lo = BCM2712_BAR4_CFG_BASE + 8U * idx;
    uint32_t off_cfg_hi = off_cfg_lo + 4U;
    uint32_t off_rmp_lo = BCM2712_BAR4_REMAP_BASE + 8U * idx;
    uint32_t off_rmp_hi = off_rmp_lo + 4U;

    uint32_t saved_cfg_lo = 0U, saved_cfg_hi = 0U, saved_rmp_lo = 0U, saved_rmp_hi = 0U;
    (void)aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_lo, &saved_cfg_lo);
    (void)aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_hi, &saved_cfg_hi);
    (void)aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_lo, &saved_rmp_lo);
    (void)aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_hi, &saved_rmp_hi);

    out->slot = (uint8_t)(4U + idx);
    out->valid = true;
    out->programmed = true;
    out->pci_addr = target_pci;
    out->cpu_addr = target_cpu;
    out->span = BCM2712_WIN_4K_SPAN;
    out->saved_cfg_lo = saved_cfg_lo;
    out->saved_cfg_hi = saved_cfg_hi;
    out->saved_remap_lo = saved_rmp_lo;
    out->saved_remap_hi = saved_rmp_hi;

    uint32_t new_cfg_lo_nosize = (uint32_t)(target_pci & 0xfffff000U);
    uint32_t new_cfg_hi = (uint32_t)(target_pci >> 32);
    uint32_t new_rmp_lo = (uint32_t)(target_cpu & 0xfffff000U) | 1U;
    uint32_t new_rmp_hi = (uint32_t)(target_cpu >> 32);
    uint32_t new_cfg_lo = new_cfg_lo_nosize | BCM2712_SIZE_4K;

    if (aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_lo, new_cfg_lo_nosize) != 0 ||
        aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_hi, new_cfg_hi) != 0 ||
        aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_rmp_hi, new_rmp_hi) != 0 ||
        aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_rmp_lo, new_rmp_lo) != 0 ||
        aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_lo, new_cfg_lo) != 0) {
        (void)aarch64_rp1_msi_window_restore(p, out);
        return -EIO;
    }

    uint32_t rb_cfg_lo = 0U, rb_cfg_hi = 0U, rb_rmp_lo = 0U, rb_rmp_hi = 0U;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_lo, &rb_cfg_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_hi, &rb_cfg_hi) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_lo, &rb_rmp_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_hi, &rb_rmp_hi) != 0 ||
        rb_cfg_lo != new_cfg_lo || rb_cfg_hi != new_cfg_hi ||
        rb_rmp_lo != new_rmp_lo || rb_rmp_hi != new_rmp_hi) {
        (void)aarch64_rp1_msi_window_restore(p, out);
        return -EIO;
    }

    log_info("[rp1-irq-window] action=program slot=%u pci=0x%lx cpu=0x%lx span=0x%lx readback=ok\n",
             (unsigned)out->slot, (unsigned long)target_pci,
             (unsigned long)target_cpu, (unsigned long)BCM2712_WIN_4K_SPAN);
    return 0;
}

int aarch64_rp1_msi_window_restore(const struct aarch64_rp1_irq_platform *p,
                                   struct aarch64_rp1_msi_window *win)
{
    if (!win || !win->valid)
        return -EINVAL;
    if (!win->programmed)
        return 0;
    if (!p || !p->valid || win->slot < 4U ||
        win->slot >= 4U + BCM2712_EXTRA_WIN_COUNT)
        return -EINVAL;

    uint32_t idx = (uint32_t)(win->slot - 4U);
    uint32_t off_cfg_lo = BCM2712_BAR4_CFG_BASE + 8U * idx;
    uint32_t off_cfg_hi = off_cfg_lo + 4U;
    uint32_t off_rmp_lo = BCM2712_BAR4_REMAP_BASE + 8U * idx;
    uint32_t off_rmp_hi = off_rmp_lo + 4U;

    /* Clear size first, then restore remap & config registers. */
    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_lo, win->saved_cfg_lo & ~0x1fU);
    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_rmp_lo, win->saved_remap_lo);
    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_rmp_hi, win->saved_remap_hi);
    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_hi, win->saved_cfg_hi);
    (void)aarch64_rp1_mmio_write32(p, RP1_MMIO_RC, off_cfg_lo, win->saved_cfg_lo);

    uint32_t rb_cfg_lo = 0U, rb_cfg_hi = 0U, rb_rmp_lo = 0U, rb_rmp_hi = 0U;
    if (aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_lo, &rb_cfg_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_cfg_hi, &rb_cfg_hi) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_lo, &rb_rmp_lo) != 0 ||
        aarch64_rp1_mmio_read32(p, RP1_MMIO_RC, off_rmp_hi, &rb_rmp_hi) != 0 ||
        rb_cfg_lo != win->saved_cfg_lo || rb_cfg_hi != win->saved_cfg_hi ||
        rb_rmp_lo != win->saved_remap_lo || rb_rmp_hi != win->saved_remap_hi) {
        s_extra_win_quarantined[idx] = true;
        win->quarantined = true;
        return -EIO;
    }

    win->valid = false;
    win->programmed = false;
    return 0;
}
