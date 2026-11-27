/* kernel/include/arch/pci.h — Architecture-specific PCI backend interface */
#ifndef _ARCH_PCI_H
#define _ARCH_PCI_H

#include <stdint.h>
#include <stdbool.h>

struct pci_device;

struct pci_root {
    uint16_t domain;
    uint8_t bus;
};

enum pci_error_scope {
    PCI_ERROR_FUNCTION = 0,
    PCI_ERROR_ROOT,
};

#define PCI_SPACE_IO      1U
#define PCI_SPACE_MMIO32  2U
#define PCI_SPACE_MMIO64  3U

struct pci_backend {
    const struct pci_root *roots;
    unsigned int root_count;
    int (*read32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset, uint32_t *out, enum pci_error_scope *scope);
    int (*write32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset, uint32_t value, enum pci_error_scope *scope);
    int (*route_gsi)(const struct pci_device *pdev, uint32_t *out);

    /* Phase-1 extensions: zero/NULL preserves legacy x86_64 behavior */
    uint16_t max_config_offset;
    int (*translate_resource)(uint16_t domain, uint8_t space,
                              uint64_t pci_addr, uint64_t size,
                              uint64_t *out_cpu_addr);
    int (*bus_range)(uint16_t domain, uint8_t *out_bus_start, uint8_t *out_bus_end);
};

const struct pci_backend *arch_pci_backend(void);

int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt);
void arch_pci_msix_unmap(const struct pci_device *pdev, void *virt);
uint32_t arch_pci_msi_address(const struct pci_device *pdev);

#endif /* _ARCH_PCI_H */
