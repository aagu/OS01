/* kernel/include/bus/pci/driver.h — PCI driver registration and callbacks */
#ifndef _BUS_PCI_DRIVER_H
#define _BUS_PCI_DRIVER_H

#include <bus/pci/pci.h>

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    unsigned int id_count;
    int (*probe)(struct pci_device *pdev, const struct pci_device_id *id);
    void (*remove)(struct pci_device *pdev);
    struct pci_driver *next;
};

int pci_register_driver(const struct pci_driver *driver);

#endif /* _BUS_PCI_DRIVER_H */
