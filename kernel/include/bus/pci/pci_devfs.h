/* kernel/include/bus/pci/pci_devfs.h — PCI devices published in the devfs namespace
 *
 * ARCH-12: every enumerated PCI device is published as
 * /dev/pci/<BDF>/<attr> so that user space can enumerate the PCI bus
 * with plain directory reads — no new syscall, no UAPI change.
 */
#ifndef _BUS_PCI_PCI_DEVFS_H
#define _BUS_PCI_PCI_DEVFS_H

#include <stdint.h>
#include <stddef.h>
#include <bus/pci/pci.h>

// Attribute ids — the order matches the registration table in pci_devfs.c.
typedef enum {
    PCI_ATTR_VENDOR = 0,
    PCI_ATTR_DEVICE,
    PCI_ATTR_CLASS,
    PCI_ATTR_CONFIG,
    PCI_ATTR_RESOURCE,
    PCI_ATTR_IRQ,
    PCI_ATTR_DRIVER,
    PCI_ATTR_COUNT,
} pci_attr_id_t;

// Register /dev/pci/<BDF>/<attr> for every enumerated PCI device.
// Returns the number of attribute nodes published, or a negative errno.
// Safe to call once after devfs is mounted; a second call is a no-op that
// returns the published count.
int pci_devfs_register_all(void);

// Register /dev/pci/<BDF>/<attr> for ONE device.  Exposed so the host test
// can publish a synthetic device without a PCI enumeration.
// Returns the number of attribute nodes PRESENT after the call (an
// already-existing node counts as present, so the call is idempotent), or a
// negative errno for an invalid argument.
int pci_devfs_register_device(const struct pci_device *pdev);

// Render one attribute of one device into `buf` (raw bytes, NOT
// NUL-terminated).  Returns the TOTAL content length in bytes — which may
// exceed `size` when the content was truncated, so the caller always
// learns the stream length — or a negative errno.
int pci_devfs_format_attr(const struct pci_device *pdev, pci_attr_id_t attr,
                           char *buf, size_t size);

#endif /* _BUS_PCI_PCI_DEVFS_H */
