/* kernel/bus/pci/pci_devfs.c — publish PCI devices in the devfs namespace
 *
 * ARCH-12.  Every enumerated PCI device becomes
 * /dev/pci/<BDF>/<attr> where <BDF> is "dddd:dd:dd.d" and <attr> is one of
 * vendor / device / class / config / resource / irq / driver.  User space
 * enumerates the bus with opendir()/readdir() — no new syscall and no UAPI
 * change, because ARCH-11 gave devfs multi-component registration paths.
 *
 * The attribute CONTENT is produced by pci_devfs_format_attr(); the devfs
 * nodes are produced by pci_devfs_register_device() / pci_devfs_register_all().
 */
#include <bus/pci/pci_devfs.h>
#include <bus/pci/driver.h>
#include <log/log.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

// Size of the stack buffer the read callback renders one attribute into.
// It must cover the largest attribute content: the configuration space is
// exactly 256 bytes and the BAR list is at most 6 lines of 42 bytes (252).
// 320 leaves headroom while keeping the read callback's stack frame below
// the kernel's -Wframe-larger-than=512 budget.
#define PCI_DEVFS_FMT_MAX   320

// PCI configuration space length in bytes.
#define PCI_DEVFS_CFG_LEN   256

// ── Individual attribute renderers ────────────────────────────
// Each returns the CONTENT length (snprintf semantics: the returned
// length is the full length even when the buffer was too small), or a
// negative errno.

static int fmt_vendor(const struct pci_device *pdev, char *buf, size_t size)
{
    int n = snprintf(buf, size, "0x%04x\n", (unsigned)(pdev->vendor & 0xffffu));
    return (n < 0) ? -EIO : n;
}

static int fmt_device(const struct pci_device *pdev, char *buf, size_t size)
{
    int n = snprintf(buf, size, "0x%04x\n", (unsigned)(pdev->device & 0xffffu));
    return (n < 0) ? -EIO : n;
}

static int fmt_class(const struct pci_device *pdev, char *buf, size_t size)
{
    int n = snprintf(buf, size, "0x%06x\n", (unsigned)(pdev->class_code & 0xffffffu));
    return (n < 0) ? -EIO : n;
}

static int fmt_irq(const struct pci_device *pdev, char *buf, size_t size)
{
    uint32_t gsi = 0;
    // pci_route_gsi() reads the device's interrupt pin and maps it to a
    // GSI; it does not mutate the device.  The PCI core declares a
    // non-const device pointer for every entry point, so the qualifier
    // is dropped here deliberately.
    if (pci_route_gsi((struct pci_device *)pdev, &gsi) != 0) {
        int n = snprintf(buf, size, "none\n");
        return (n < 0) ? -EIO : n;
    }
    int n = snprintf(buf, size, "%u\n", (unsigned)gsi);
    return (n < 0) ? -EIO : n;
}

static int fmt_driver(const struct pci_device *pdev, char *buf, size_t size)
{
    const char *name = (pdev->driver && pdev->driver->name) ? pdev->driver->name : "none";
    int n = snprintf(buf, size, "%s\n", name);
    return (n < 0) ? -EIO : n;
}

// The BAR list.  Only the ADDRESS and the KIND are reported: the BAR
// capacity is not knowable without a destructive sizing probe, and the
// ARCH-9 driver-model contract forbids that — a BAR "length" there is the
// window the driver knows, not the BAR capacity.  Reporting a fabricated
// size would be misleading, so the "(size=...)" field Linux prints is
// deliberately omitted.
static int fmt_resource(const struct pci_device *pdev, char *buf, size_t size)
{
    size_t off = 0;
    int total = 0;

    for (int i = 0; i < 6; i++) {
        const struct pci_bar *bar = &pdev->bars[i];
        if (!bar->valid) continue;

        const char *kind;
        switch (bar->kind) {
        case PCI_BAR_IO:              kind = "I/O ports"; break;
        case PCI_BAR_MMIO32:
        case PCI_BAR_MMIO64:          kind = "Memory";    break;
        default:                      continue;   /* NONE / UPPER half */
        }

        size_t remain = (off < size) ? (size - off) : 0;
        int n = snprintf((off < size) ? (buf + off) : NULL, remain,
                         "Region %d: %s at 0x%llx\n", i, kind,
                         (unsigned long long)bar->address);
        if (n < 0) return -EIO;
        total += n;
        off += (size_t)n;
    }
    return total;
}

// The raw 256-byte configuration space, little-endian, exactly as the
// hardware returns it.
static int fmt_config(const struct pci_device *pdev, char *buf, size_t size)
{
    size_t want = (size < PCI_DEVFS_CFG_LEN) ? size : PCI_DEVFS_CFG_LEN;

    for (size_t i = 0; i < want; i += 4) {
        uint32_t dword = 0;
        // pci_config_read32() only reads the configuration space; it does
        // not mutate the device.  The PCI core declares a non-const
        // device pointer for every entry point, so the qualifier is
        // dropped here deliberately.
        int rc = pci_config_read32((struct pci_device *)pdev, (uint16_t)i, &dword);
        if (rc != 0) return rc;
        uint8_t b[4] = {
            (uint8_t)(dword & 0xffu),
            (uint8_t)((dword >> 8) & 0xffu),
            (uint8_t)((dword >> 16) & 0xffu),
            (uint8_t)((dword >> 24) & 0xffu),
        };
        for (size_t k = 0; k < 4 && (i + k) < want; k++) {
            buf[i + k] = (char)b[k];
        }
    }
    return PCI_DEVFS_CFG_LEN;
}

// ── Public formatting entry point ─────────────────────────────
int pci_devfs_format_attr(const struct pci_device *pdev, pci_attr_id_t attr,
                           char *buf, size_t size)
{
    if (!pdev || !buf) return -EINVAL;
    if ((unsigned)attr >= (unsigned)PCI_ATTR_COUNT) return -EINVAL;

    switch (attr) {
    case PCI_ATTR_VENDOR:   return fmt_vendor(pdev, buf, size);
    case PCI_ATTR_DEVICE:   return fmt_device(pdev, buf, size);
    case PCI_ATTR_CLASS:    return fmt_class(pdev, buf, size);
    case PCI_ATTR_CONFIG:   return fmt_config(pdev, buf, size);
    case PCI_ATTR_RESOURCE: return fmt_resource(pdev, buf, size);
    case PCI_ATTR_IRQ:      return fmt_irq(pdev, buf, size);
    case PCI_ATTR_DRIVER:   return fmt_driver(pdev, buf, size);
    default:                return -EINVAL;
    }
}
