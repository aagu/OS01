/* kernel/include/bus/pci/pci.h — Architecture-agnostic PCI subsystem core definitions */
#ifndef _BUS_PCI_PCI_H
#define _BUS_PCI_PCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <device/device.h>

#define PCI_ID_ANY UINT32_MAX
#define BUS_UNAVAILABLE (-ENODEV)

enum pci_bar_kind {
    PCI_BAR_NONE = 0,
    PCI_BAR_IO,
    PCI_BAR_MMIO32,
    PCI_BAR_MMIO64,
    PCI_BAR_UPPER,
};

struct pci_bar {
    enum pci_bar_kind kind;
    uint64_t address;
    bool valid;
    uint8_t index;
};

struct pci_device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class_value;
    uint32_t class_mask;
};

struct pci_driver;

struct pci_device {
    struct device dev;
    uint16_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t fn;
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class_code; /* 24-bit: (class << 16) | (subclass << 8) | prog_if */
    struct pci_bar bars[6];
    const struct pci_driver *driver;
    void *driver_data;
    bool enum_error;
    struct pci_device *next;
};

int pci_enumerate(void);
int pci_bind_all(void);
struct pci_device *pci_device_get(unsigned int index);
unsigned int pci_device_count(void);
struct pci_device *pci_device_lookup(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn);
const struct pci_device_id *pci_match_id(const struct pci_driver *drv, const struct pci_device *pdev);
int pci_bar_window(const struct pci_device *pdev, unsigned int index, enum pci_bar_kind required,
                   uint64_t offset, uint64_t length, uint64_t *physical);
int pci_config_read32(struct pci_device *pdev, uint16_t offset, uint32_t *out);
int pci_config_write32(struct pci_device *pdev, uint16_t offset, uint32_t value);
int pci_set_bus_master(struct pci_device *pdev, bool enabled);
int pci_set_intx(struct pci_device *pdev, bool enabled);
int pci_set_decode(struct pci_device *pdev, bool io, bool mmio);
int pci_route_gsi(struct pci_device *pdev, uint32_t *out);
int pci_msix_enable(struct pci_device *pdev, uint8_t vector);
int pci_interrupts_disable(struct pci_device *pdev);

#ifdef OS01_HOST_TEST
void pci_core_reset_for_test(void);
#endif

#endif /* _BUS_PCI_PCI_H */
