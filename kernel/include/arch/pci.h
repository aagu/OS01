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

struct pci_backend {
    const struct pci_root *roots;
    unsigned int root_count;
    int (*read32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset, uint32_t *out, enum pci_error_scope *scope);
    int (*write32)(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset, uint32_t value, enum pci_error_scope *scope);
    int (*route_gsi)(const struct pci_device *pdev, uint32_t *out);
};

const struct pci_backend *arch_pci_backend(void);

#endif /* _ARCH_PCI_H */
