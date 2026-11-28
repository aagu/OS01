/* kernel/arch/aarch64/bus/rp1_pcie_hw.c — Stage 3 RP1/MIP MMIO prepare & bounded access */
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/boot_mmio.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#ifndef OS01_HOST_TEST
#include <arch/spinlock.h>
#endif
#include <errno.h>
#include <log/log.h>
#include <stddef.h>
#include <string.h>

#define BCM2712_EXT_CFG_DATA  0x8000U
#define BCM2712_EXT_CFG_INDEX 0x9000U
#define RP1_APB_IRQ_STRIDE    4U
#define RP1_APB_IRQ_BASE      0x08U

#ifdef OS01_HOST_TEST
void *aarch64_rp1_test_pa_to_va(uint64_t pa);
#define rp1_pa_to_va(pa) aarch64_rp1_test_pa_to_va(pa)
#else
#define rp1_pa_to_va(pa) ((void *)(uintptr_t)(ARCH_PAGE_OFFSET + (pa)))
#endif

static struct aarch64_rp1_irq_platform s_platform;
static spinlock_T s_cfg_lock = { 1 };

static inline uint32_t hw_read32(volatile const uint8_t *base, uint32_t off)
{
    uint32_t val;
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("ldr %w0, [%1]\n\tdsb ld"
                         : "=r"(val) : "r"(base + off) : "memory");
#else
    val = *(volatile const uint32_t *)(base + off);
#endif
    return val;
}

static inline void hw_write32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("str %w0, [%1]\n\tdsb st"
                         :: "r"(val), "r"(base + off) : "memory");
#else
    *(volatile uint32_t *)(base + off) = val;
#endif
}

static inline uint16_t hw_read16(volatile const uint8_t *base, uint32_t off)
{
    uint16_t val;
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("ldrh %w0, [%1]\n\tdsb ld"
                         : "=r"(val) : "r"(base + off) : "memory");
#else
    val = *(volatile const uint16_t *)(base + off);
#endif
    return val;
}

static inline void hw_write16(volatile uint8_t *base, uint32_t off, uint16_t val)
{
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("strh %w0, [%1]\n\tdsb st"
                         :: "r"(val), "r"(base + off) : "memory");
#else
    *(volatile uint16_t *)(base + off) = val;
#endif
}

static uint32_t rc_io_read32(void *ctx, uint32_t off)
{
    return hw_read32((volatile const uint8_t *)ctx, off);
}

static uint32_t rc_io_cfg_save_select(void *ctx, uint32_t bdf)
{
    volatile uint8_t *base = (volatile uint8_t *)ctx;
    uint32_t prev = hw_read32(base, BCM2712_EXT_CFG_INDEX);
    hw_write32(base, BCM2712_EXT_CFG_INDEX, bdf);
    return prev;
}

static void rc_io_cfg_restore(void *ctx, uint32_t saved)
{
    hw_write32((volatile uint8_t *)ctx, BCM2712_EXT_CFG_INDEX, saved);
}

void aarch64_rp1_pcie_prepare(const struct boot_context *ctx)
{
    memset(&s_platform, 0, sizeof(s_platform));

    const struct aarch64_rp1_irq_desc *desc = aarch64_rp1_irq_desc_get();
    const struct boot_rp1_pci_info *pci = boot_context_rp1_pci(ctx);
    if (!desc || !desc->valid || !pci || pci->entries == 0U ||
        pci->bar0_size == 0ULL)
        return;

    struct aarch64_boot_mmio_ctx mmio_ctx;
    if (aarch64_boot_mmio_ctx_init(ctx, &mmio_ctx) != 0)
        return;
    if (aarch64_boot_mmio_map(&mmio_ctx, desc->rc_reg_base, desc->rc_reg_size) != 0)
        return;

    volatile uint8_t *rc_va = (volatile uint8_t *)rp1_pa_to_va(desc->rc_reg_base);
    if (!rc_va)
        return;

    struct aarch64_rp1_pcie_io io = {
        .ctx = (void *)(uintptr_t)rc_va,
        .rc_read32 = rc_io_read32,
        .cfg_save_select = rc_io_cfg_save_select,
        .cfg_restore = rc_io_cfg_restore,
    };
    struct aarch64_rp1_endpoint ep;
    if (aarch64_rp1_pcie_endpoint(desc, &io, &ep) != 0)
        return;

    /* Cross-check live endpoint against UEFI handoff before touching table/PBA. */
    if (ep.bus != pci->bus || ep.device != pci->device ||
        ep.function != pci->function || ep.bar0_pci != pci->bar0_pci ||
        ep.bar0_cpu != pci->bar0_cpu)
        return;

    if (pci->msix_cap < 0x40U || pci->msix_cap > 0xf4U || (pci->msix_cap & 3U) != 0U)
        return;

    uint32_t bdf = ((uint32_t)ep.bus << 20) | ((uint32_t)ep.device << 15) |
                   ((uint32_t)ep.function << 12);
    uint32_t saved_sel = rc_io_cfg_save_select((void *)(uintptr_t)rc_va, bdf);
    uint32_t cap_dw0 = hw_read32(rc_va, BCM2712_EXT_CFG_DATA + pci->msix_cap);
    uint32_t cap_dw1 = hw_read32(rc_va, BCM2712_EXT_CFG_DATA + pci->msix_cap + 4U);
    uint32_t cap_dw2 = hw_read32(rc_va, BCM2712_EXT_CFG_DATA + pci->msix_cap + 8U);
    rc_io_cfg_restore((void *)(uintptr_t)rc_va, saved_sel);

    uint16_t live_entries = (uint16_t)(((cap_dw0 >> 16) & 0x7ffU) + 1U);
    if ((cap_dw0 & 0xffU) != 0x11U || live_entries != pci->entries ||
        (cap_dw1 & 7U) != 0U || (cap_dw1 & ~7U) != pci->table_offset ||
        (cap_dw2 & 7U) != 0U || (cap_dw2 & ~7U) != pci->pba_offset)
        return;

    uint64_t table_span = (uint64_t)pci->entries * 16ULL;
    uint64_t pba_span = ((uint64_t)(pci->entries + 63U) / 64ULL) * 8ULL;
    if ((uint64_t)pci->table_offset > pci->bar0_size ||
        table_span > pci->bar0_size - pci->table_offset ||
        (uint64_t)pci->pba_offset > pci->bar0_size ||
        pba_span > pci->bar0_size - pci->pba_offset ||
        pci->bar0_pci + pci->bar0_size > ep.outbound_pci_base + ep.outbound_size)
        return;

    uint64_t table_pa = ep.bar0_cpu + pci->table_offset;
    uint64_t pba_pa = ep.bar0_cpu + pci->pba_offset;
    uint64_t apb_pa = ep.bar1_cpu + AARCH64_RP1_APB_OFFSET;

    if (aarch64_boot_mmio_map(&mmio_ctx, desc->mip_cpu_pa, desc->mip_span) != 0 ||
        aarch64_boot_mmio_map(&mmio_ctx, table_pa, table_span) != 0 ||
        aarch64_boot_mmio_map(&mmio_ctx, pba_pa, pba_span) != 0 ||
        aarch64_boot_mmio_map(&mmio_ctx, apb_pa, AARCH64_RP1_APB_SPAN) != 0)
        return;

    volatile uint8_t *mip_va = (volatile uint8_t *)rp1_pa_to_va(desc->mip_cpu_pa);
    volatile uint8_t *apb_va = (volatile uint8_t *)rp1_pa_to_va(apb_pa);
    volatile uint8_t *table_va = (volatile uint8_t *)rp1_pa_to_va(table_pa);
    volatile uint8_t *pba_va = (volatile uint8_t *)rp1_pa_to_va(pba_pa);
    if (!mip_va || !apb_va || !table_va || !pba_va)
        return;

    s_platform.desc = *desc;
    s_platform.endpoint = ep;
    s_platform.bar0 = *pci;
    s_platform.endpoint_bdf = bdf;
    s_platform.rc_va = rc_va;
    s_platform.mip_va = mip_va;
    s_platform.apb_va = apb_va;
    s_platform.table_va = table_va;
    s_platform.pba_va = pba_va;
    s_platform.rc_span = (uint32_t)desc->rc_reg_size;
    s_platform.mip_span = (uint32_t)desc->mip_span;
    s_platform.apb_span = (uint32_t)AARCH64_RP1_APB_SPAN;
    s_platform.table_span = (uint32_t)table_span;
    s_platform.pba_span = (uint32_t)pba_span;
    s_platform.valid = true;

    log_info("[rp1-irq-topology] rc=0x%lx mip=0x%lx source=%u spi_base=%u count=%u offset=%u\n",
             (unsigned long)desc->rc_reg_base, (unsigned long)desc->mip_cpu_pa,
             (unsigned)desc->uart_source, (unsigned)desc->base_spi,
             (unsigned)desc->count, (unsigned)desc->offset);
    log_info("[rp1-irq-bar0] bdf=%02x:%02x.%x pci=0x%lx cpu=0x%lx size=0x%lx table=0x%x pba=0x%x\n",
             (unsigned)ep.bus, (unsigned)ep.device, (unsigned)ep.function,
             (unsigned long)pci->bar0_pci, (unsigned long)pci->bar0_cpu,
             (unsigned long)pci->bar0_size, (unsigned)pci->table_offset,
             (unsigned)pci->pba_offset);
}

const struct aarch64_rp1_irq_platform *aarch64_rp1_irq_platform_get(void)
{
    return s_platform.valid ? &s_platform : NULL;
}

static int mmio_resolve(const struct aarch64_rp1_irq_platform *p,
                        enum aarch64_rp1_mmio_space space, uint32_t off,
                        bool is_write, volatile uint8_t **base_out)
{
    volatile uint8_t *base = NULL;
    uint32_t span = 0U;

    if (!p || !p->valid || (off & 3U) != 0U)
        return -EINVAL;
    switch (space) {
    case RP1_MMIO_RC:    base = p->rc_va; span = p->rc_span; break;
    case RP1_MMIO_MIP:   base = p->mip_va; span = p->mip_span; break;
    case RP1_MMIO_APB:   base = p->apb_va; span = p->apb_span; break;
    case RP1_MMIO_TABLE: base = p->table_va; span = p->table_span; break;
    case RP1_MMIO_PBA:
        if (is_write)
            return -EACCES;
        base = p->pba_va;
        span = p->pba_span;
        break;
    default:
        return -EINVAL;
    }
    if (!base || span < 4U || off > span - 4U)
        return -ERANGE;
    *base_out = base;
    return 0;
}

int aarch64_rp1_mmio_read32(const struct aarch64_rp1_irq_platform *p,
                            enum aarch64_rp1_mmio_space space,
                            uint32_t off, uint32_t *value)
{
    volatile uint8_t *base = NULL;
    if (!value)
        return -EINVAL;
    int rc = mmio_resolve(p, space, off, false, &base);
    if (rc != 0)
        return rc;
    *value = hw_read32(base, off);
    return 0;
}

int aarch64_rp1_mmio_write32(const struct aarch64_rp1_irq_platform *p,
                             enum aarch64_rp1_mmio_space space,
                             uint32_t off, uint32_t value)
{
    volatile uint8_t *base = NULL;
    int rc = mmio_resolve(p, space, off, true, &base);
    if (rc != 0)
        return rc;
    hw_write32(base, off, value);
    return 0;
}

int aarch64_rp1_cfg_read16(const struct aarch64_rp1_irq_platform *p,
                           uint16_t off, uint16_t *value)
{
    if (!p || !p->valid || !p->rc_va || !value || (off & 1U) != 0U || off >= 256U)
        return -EINVAL;
    unsigned long flags = spin_lock_irqsave(&s_cfg_lock);
    uint32_t saved = rc_io_cfg_save_select((void *)(uintptr_t)p->rc_va, p->endpoint_bdf);
    uint16_t v = hw_read16(p->rc_va, BCM2712_EXT_CFG_DATA + off);
    rc_io_cfg_restore((void *)(uintptr_t)p->rc_va, saved);
    spin_unlock_irqrestore(&s_cfg_lock, flags);
    *value = v;
    return 0;
}

int aarch64_rp1_cfg_write16(const struct aarch64_rp1_irq_platform *p,
                            uint16_t off, uint16_t value)
{
    if (!p || !p->valid || !p->rc_va || (off & 1U) != 0U || off >= 256U)
        return -EINVAL;
    unsigned long flags = spin_lock_irqsave(&s_cfg_lock);
    uint32_t saved = rc_io_cfg_save_select((void *)(uintptr_t)p->rc_va, p->endpoint_bdf);
    hw_write16(p->rc_va, BCM2712_EXT_CFG_DATA + off, value);
    rc_io_cfg_restore((void *)(uintptr_t)p->rc_va, saved);
    spin_unlock_irqrestore(&s_cfg_lock, flags);
    return 0;
}

int aarch64_rp1_irq_flush(const struct aarch64_rp1_irq_platform *p,
                          uint32_t source)
{
    uint32_t dummy = 0U;
    if (source >= AARCH64_RP1_NUM_IRQS)
        return -EINVAL;
    return aarch64_rp1_mmio_read32(p, RP1_MMIO_APB,
                                   RP1_APB_IRQ_BASE + source * RP1_APB_IRQ_STRIDE,
                                   &dummy);
}
