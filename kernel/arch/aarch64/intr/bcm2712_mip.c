/* kernel/arch/aarch64/intr/bcm2712_mip.c — BCM2712 MSI-X Input Peripheral (MIP) slot allocator */
#include <arch/aarch64/bcm2712_mip.h>
#include <arch/aarch64/ipi.h>
#include <log/log.h>
#include <errno.h>
#include <string.h>

#define MIP_INT_CFGL_HOST    0x20U
#define MIP_INT_CFGH_HOST    0x30U
#define MIP_INT_MASKL_HOST   0x40U
#define MIP_INT_MASKH_HOST   0x50U
#define MIP_INT_MASKL_VPU    0x60U
#define MIP_INT_MASKH_VPU    0x70U
#define MIP_INT_STATUSL_HOST 0x80U
#define MIP_INT_STATUSH_HOST 0x90U
#define MIP_INT_STATUSL_VPU  0xa0U
#define MIP_INT_STATUSH_VPU  0xb0U

#define MIP_MAX_SLOTS 64U

static bool s_slot_in_use[MIP_MAX_SLOTS];
static bool s_slot_quarantined[MIP_MAX_SLOTS];

static int mip_reg_read_bit(const struct aarch64_rp1_irq_platform *p,
                            uint32_t lo_off, uint32_t hw_slot, bool *bit_out)
{
    uint32_t off = lo_off + (hw_slot >= 32U ? 0x10U : 0U);
    uint32_t v = 0U;
    int rc = aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, off, &v);
    if (rc != 0)
        return rc;
    *bit_out = (v & (1U << (hw_slot % 32U))) != 0U;
    return 0;
}

static int mip_reg_write_bit(const struct aarch64_rp1_irq_platform *p,
                             uint32_t lo_off, uint32_t hw_slot, bool set)
{
    uint32_t off = lo_off + (hw_slot >= 32U ? 0x10U : 0U);
    uint32_t bit = 1U << (hw_slot % 32U);
    uint32_t v = 0U;
    int rc = aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, off, &v);
    if (rc != 0)
        return rc;
    if (set) v |= bit;
    else     v &= ~bit;
    rc = aarch64_rp1_mmio_write32(p, RP1_MMIO_MIP, off, v);
    if (rc != 0)
        return rc;
    uint32_t rb = 0U;
    rc = aarch64_rp1_mmio_read32(p, RP1_MMIO_MIP, off, &rb);
    if (rc != 0)
        return rc;
    return ((rb & bit) != 0U) == set ? 0 : -EIO;
}

static int mip_restore_hw_slot(const struct aarch64_rp1_irq_platform *p,
                               uint32_t hw_slot, bool edge,
                               bool host_mask, bool vpu_mask)
{
    if (mip_reg_write_bit(p, MIP_INT_MASKL_HOST, hw_slot, host_mask) != 0 ||
        mip_reg_write_bit(p, MIP_INT_MASKL_VPU, hw_slot, vpu_mask) != 0 ||
        mip_reg_write_bit(p, MIP_INT_CFGL_HOST, hw_slot, edge) != 0)
        return -EIO;
    return 0;
}

int bcm2712_mip_reserve(const struct aarch64_rp1_irq_platform *p,
                        gic_handler_fn handler, uint64_t param,
                        struct bcm2712_mip_route *out)
{
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!p || !p->valid || !p->desc.valid || !handler)
        return -EINVAL;

    struct gic_dev *dev = gic_dev_current();
    uint8_t bsp_target = gic_target_bit_get(0U);
    if (!dev || dev->is_v3 || bsp_target == 0U)
        return -ENODEV;

    uint32_t base_spi = p->desc.base_spi;
    uint32_t offset = p->desc.offset;
    uint32_t count = p->desc.count;
    if (count == 0U || offset >= MIP_MAX_SLOTS || count > MIP_MAX_SLOTS - offset)
        return -EINVAL;
    if ((uint64_t)GIC_INTID_SPI_FIRST + base_spi + offset + count > dev->nr_intids)
        return -ERANGE;

    for (uint32_t slot = 0U; slot < count; slot++) {
        uint32_t hw_slot = offset + slot;
        uint32_t intid = GIC_INTID_SPI_FIRST + base_spi + hw_slot;
        if (s_slot_in_use[hw_slot] || s_slot_quarantined[hw_slot])
            continue;
        if (gic_get_handler(intid, NULL) != NULL)
            continue;

        struct gic_spi_state st;
        if (gic_spi_get_state(dev, intid, &st) != 0)
            continue;
        if (st.enable || st.active || st.pending)
            continue;

        bool host_pend = false, vpu_pend = false;
        if (mip_reg_read_bit(p, MIP_INT_STATUSL_HOST, hw_slot, &host_pend) != 0 ||
            mip_reg_read_bit(p, MIP_INT_STATUSL_VPU, hw_slot, &vpu_pend) != 0)
            return -EIO;
        if (host_pend || vpu_pend)
            continue;

        bool saved_edge = false, saved_hmask = false, saved_vmask = false;
        if (mip_reg_read_bit(p, MIP_INT_CFGL_HOST, hw_slot, &saved_edge) != 0 ||
            mip_reg_read_bit(p, MIP_INT_MASKL_HOST, hw_slot, &saved_hmask) != 0 ||
            mip_reg_read_bit(p, MIP_INT_MASKL_VPU, hw_slot, &saved_vmask) != 0)
            return -EIO;

        if (gic_register_handler(intid, handler, param, "bcm2712-mip") != 0)
            continue;

        s_slot_in_use[hw_slot] = true;
        out->valid = true;
        out->plat = p;
        out->slot = slot;
        out->hw_slot = hw_slot;
        out->intid = intid;
        out->address = p->desc.msi_pci_addr;
        out->data = hw_slot;
        out->handler = handler;
        out->param = param;
        out->saved_gic = st;
        out->saved_mip_edge = saved_edge;
        out->saved_mip_host_mask = saved_hmask;
        out->saved_mip_vpu_mask = saved_vmask;

        if (mip_reg_write_bit(p, MIP_INT_CFGL_HOST, hw_slot, true) != 0 ||
            mip_reg_write_bit(p, MIP_INT_MASKL_VPU, hw_slot, true) != 0 ||
            mip_reg_write_bit(p, MIP_INT_MASKL_HOST, hw_slot, false) != 0 ||
            gic_spi_set_edge(dev, intid, true) != 0 ||
            gic_irq_config(dev, intid, false, 0x00U, bsp_target) != 0) {
            if (mip_restore_hw_slot(p, hw_slot, saved_edge, saved_hmask, saved_vmask) != 0 ||
                gic_spi_restore_state(dev, intid, &st) != 0) {
                s_slot_quarantined[hw_slot] = true;
                out->quarantined = true;
                return -EIO;
            }
            (void)gic_unregister_handler(intid);
            s_slot_in_use[hw_slot] = false;
            memset(out, 0, sizeof(*out));
            return -EIO;
        }
        return 0;
    }

    return -ENOSPC;
}

int bcm2712_mip_enable(struct bcm2712_mip_route *route)
{
    if (!route || !route->valid || route->quarantined)
        return -EINVAL;
    struct gic_dev *dev = gic_dev_current();
    if (!dev || !dev->gicd)
        return -ENODEV;
    if (gic_irq_enable(dev, route->intid) != 0)
        return -EIO;
    struct gic_spi_state st;
    if (gic_spi_get_state(dev, route->intid, &st) != 0 || !st.enable) {
        if (gic_irq_disable(dev, route->intid) != 0 ||
            gic_spi_get_state(dev, route->intid, &st) != 0 || st.enable) {
            s_slot_quarantined[route->hw_slot] = true;
            route->quarantined = true;
        }
        return -EIO;
    }
    return 0;
}

int bcm2712_mip_disable(struct bcm2712_mip_route *route)
{
    if (!route || !route->valid)
        return -EINVAL;
    struct gic_dev *dev = gic_dev_current();
    if (!dev || !dev->gicd)
        return -ENODEV;
    if (gic_irq_disable(dev, route->intid) != 0)
        return -EIO;
    struct gic_spi_state st;
    if (gic_spi_get_state(dev, route->intid, &st) != 0 || st.enable)
        return -EIO;
    return 0;
}

bool bcm2712_mip_pending(const struct bcm2712_mip_route *route)
{
    if (!route || !route->valid)
        return false;
    struct gic_dev *dev = gic_dev_current();
    if (!dev || !dev->gicd)
        return false;
    struct gic_spi_state st;
    if (gic_spi_get_state(dev, route->intid, &st) != 0)
        return false;
    return st.pending;
}

int bcm2712_mip_clear_probe(struct bcm2712_mip_route *route)
{
    if (!route || !route->valid || !route->plat)
        return -EINVAL;
    struct gic_dev *dev = gic_dev_current();
    if (!dev || !dev->gicd)
        return -ENODEV;

    gic_clear_pending(dev, route->intid);
    struct gic_spi_state st;
    if (gic_spi_get_state(dev, route->intid, &st) != 0 || st.pending) {
        log_warn("[bcm2712-mip] clear_probe gic_pending=%u active=%u en=%u\n",
                 (unsigned)st.pending, (unsigned)st.active, (unsigned)st.enable);
        return -EIO;
    }

    bool host_pend = true;
    if (mip_reg_read_bit(route->plat, MIP_INT_STATUSL_HOST, route->hw_slot, &host_pend) != 0)
        return -EIO;
    if (host_pend) {
        log_warn("[bcm2712-mip] clear_probe host_pend=%u\n", (unsigned)host_pend);
        return -EIO;
    }
    return 0;
}

int bcm2712_mip_release(struct bcm2712_mip_route *route, bool quiescent)
{
    if (!route || !route->valid || !route->plat)
        return -EINVAL;
    struct gic_dev *dev = gic_dev_current();
    if (!dev || !dev->gicd)
        return -ENODEV;

    if (bcm2712_mip_disable(route) != 0 || !quiescent) {
        (void)mip_reg_write_bit(route->plat, MIP_INT_MASKL_HOST, route->hw_slot, true);
        s_slot_quarantined[route->hw_slot] = true;
        route->quarantined = true;
        return -EIO;
    }

    if (mip_restore_hw_slot(route->plat, route->hw_slot,
                            route->saved_mip_edge,
                            route->saved_mip_host_mask,
                            route->saved_mip_vpu_mask) != 0 ||
        gic_spi_restore_state(dev, route->intid, &route->saved_gic) != 0) {
        (void)mip_reg_write_bit(route->plat, MIP_INT_MASKL_HOST, route->hw_slot, true);
        s_slot_quarantined[route->hw_slot] = true;
        route->quarantined = true;
        return -EIO;
    }

    (void)gic_unregister_handler(route->intid);
    s_slot_in_use[route->hw_slot] = false;
    route->valid = false;
    return 0;
}
