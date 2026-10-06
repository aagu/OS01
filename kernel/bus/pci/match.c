/* kernel/bus/pci/match.c — PCI device-driver matching engine */
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

const struct pci_device_id *pci_match_id(const struct pci_driver *drv, const struct pci_device *pdev)
{
    if (!drv || !pdev || !drv->id_table || drv->id_count == 0) {
        return NULL;
    }

    for (unsigned int i = 0; i < drv->id_count; i++) {
        const struct pci_device_id *id = &drv->id_table[i];

        if (id->vendor != PCI_ID_ANY && id->vendor != pdev->vendor) {
            continue;
        }
        if (id->device != PCI_ID_ANY && id->device != pdev->device) {
            continue;
        }
        if (id->subvendor != PCI_ID_ANY && id->subvendor != pdev->subvendor) {
            continue;
        }
        if (id->subdevice != PCI_ID_ANY && id->subdevice != pdev->subdevice) {
            continue;
        }
        if (id->class_mask != 0) {
            if ((pdev->class_code & id->class_mask) != (id->class_value & id->class_mask)) {
                continue;
            }
        }

        /* First matching entry in driver table order */
        return id;
    }

    return NULL;
}
