/* kernel/arch/x86_64/bus/pci.c — x86_64 PCI backend and platform services */
#include <arch/pci.h>
#include <arch/x86_64/pci.h>
#include <bus/pci/pci.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef OS01_HOST_TEST
#include <arch/io.h>
#include <arch/spinlock.h>
#include <memory/memory.h>   // Phy_To_Virt
#include <memory/pmm.h>      // PAGE_2M_MASK
#include <memory/vmm.h>      // vmm_map_page
#include <arch/x86_64/pte.h> // PAGE_KERNEL_PMD_NOCACHE
#endif

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static spinlock_T s_pci_lock = { .lock = 1L };

static inline uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset)
{
    return 0x80000000U
         | ((uint32_t)bus << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(fn & 0x07) << 8)
         | (uint32_t)(offset & 0xFC);
}

static int x86_pci_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                          uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    if (domain != 0) {
        if (scope) {
            *scope = PCI_ERROR_ROOT;
        }
        return -ENODEV;
    }
    if (slot >= 32 || fn >= 8) {
        return -EINVAL;
    }
    if (!out || (offset & 3) != 0 || offset > 252) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&s_pci_lock);
    arch_outd(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, fn, offset));
    *out = arch_ind(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&s_pci_lock, flags);

    return 0;
}

static int x86_pci_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                           uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    if (domain != 0) {
        if (scope) {
            *scope = PCI_ERROR_ROOT;
        }
        return -ENODEV;
    }
    if (slot >= 32 || fn >= 8) {
        return -EINVAL;
    }
    if ((offset & 3) != 0 || offset > 252) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&s_pci_lock);
    arch_outd(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, fn, offset));
    arch_outd(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&s_pci_lock, flags);

    return 0;
}

static int x86_pci_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    if (!pdev || !out) {
        return -EINVAL;
    }

    enum pci_error_scope scope = PCI_ERROR_FUNCTION;
    uint32_t reg = 0;
    int rc = x86_pci_read32(pdev->domain, pdev->bus, pdev->slot, pdev->fn, 0x3C, &reg, &scope);
    if (rc != 0) {
        return rc;
    }

    uint8_t int_line = (uint8_t)(reg & 0xFF);
    uint8_t int_pin  = (uint8_t)((reg >> 8) & 0xFF);

    if (int_pin == 0) {
        *out = int_line;
        return 0;
    }

    /* Q35/ICH9 routing: PIRQ[A-D] -> GSI[16-19], PIRQ = (slot + pin - 1) & 3 */
    uint32_t gsi = 16 + ((pdev->slot + int_pin - 1) & 3);
    if (int_line >= 16) {
        *out = int_line;
    } else {
        *out = gsi;
    }

    return 0;
}

static const struct pci_root s_x86_roots[] = {
    { .domain = 0, .bus = 0 },
};

static const struct pci_backend s_x86_pci_backend = {
    .roots = s_x86_roots,
    .root_count = 1,
    .read32 = x86_pci_read32,
    .write32 = x86_pci_write32,
    .route_gsi = x86_pci_route_gsi,
};

const struct pci_backend *x86_pci_backend(void)
{
    return &s_x86_pci_backend;
}

const struct pci_backend *arch_pci_backend(void)
{
    return &s_x86_pci_backend;
}

#ifndef OS01_HOST_TEST
int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt)
{
    (void)pdev;
    if (!out_virt) {
        return -EINVAL;
    }
    uint64_t table_page = table_phys & PAGE_2M_MASK;
    vmm_map_page(kernel_map, table_page,
                 (uintptr_t)Phy_To_Virt(table_page),
                 PAGE_KERNEL_PMD_NOCACHE);
    *out_virt = (void *)Phy_To_Virt(table_phys);
    return 0;
}

void arch_pci_msix_unmap(const struct pci_device *pdev, void *virt)
{
    (void)pdev;
    (void)virt;
}

uint32_t arch_pci_msi_address(const struct pci_device *pdev)
{
    (void)pdev;
    extern uint32_t lapic_read(uint32_t offset);
    uint32_t bsp_lapic_id = (lapic_read(0x020) >> 24) & 0xFF;
    return 0xFEE00000 | (bsp_lapic_id << 12);
}
#endif
