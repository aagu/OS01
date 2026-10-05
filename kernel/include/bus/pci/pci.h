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

#ifdef OS01_HOST_TEST
void pci_core_reset_for_test(void);
#endif

#endif /* _BUS_PCI_PCI_H */
