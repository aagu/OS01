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
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <log/log.h>
#include <memory/slab.h>
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

// The devfs namespace and the per-device directory name.
#define PCI_DEVFS_NAMESPACE   "pci"
#define PCI_DEVFS_BDF_MAX     32    // "0000:00:1f.2" + NUL
#define PCI_DEVFS_PATH_MAX    64    // "pci/" + BDF + "/" + attribute + NUL

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

// ── devfs publication ─────────────────────────────────────────

// Per-attribute context: the devfs node's private_data.  One context
// per attribute node — the node has to know WHICH attribute it serves.
typedef struct pci_attr_ctx {
    struct pci_device *pdev;
    uint8_t            attr;      // pci_attr_id_t
} pci_attr_ctx_t;

static const char *const s_attr_names[PCI_ATTR_COUNT] = {
    "vendor", "device", "class", "config", "resource", "irq", "driver",
};

// One devfs_ops table for every attribute; the read callback dispatches
// on the context's attribute id.
static int pci_attr_read(vfs_node_t *node, uint64_t offset, uint64_t size,
                          void *buffer)
{
    pci_attr_ctx_t *ctx = (pci_attr_ctx_t *)devfs_get_private(node);
    if (!ctx || !ctx->pdev || !buffer) return -EINVAL;
    if (size == 0) return 0;

    char tmp[PCI_DEVFS_FMT_MAX];
    int len = pci_devfs_format_attr(ctx->pdev, (pci_attr_id_t)ctx->attr,
                                     tmp, sizeof(tmp));
    if (len < 0) return len;
    if (offset >= (uint64_t)len) return 0;

    uint64_t avail = (uint64_t)len - offset;
    uint64_t n = (size < avail) ? size : avail;
    memcpy(buffer, tmp + offset, n);
    return (int)n;
}

static const struct devfs_ops pci_attr_ops = {
    .read = pci_attr_read,
};

// Publish /dev/pci/<BDF>/<attr> for one device.  Returns the number of
// attribute nodes present afterwards; an already-existing node counts as
// present, so the call is idempotent.
int pci_devfs_register_device(const struct pci_device *pdev)
{
    if (!pdev) return -EINVAL;

    char bdf[PCI_DEVFS_BDF_MAX];
    int n = snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%u",
                     (unsigned)pdev->domain, (unsigned)pdev->bus,
                     (unsigned)pdev->slot, (unsigned)pdev->fn);
    if (n < 0 || (size_t)n >= sizeof(bdf)) return -EINVAL;

    int published = 0;
    for (int i = 0; i < PCI_ATTR_COUNT; i++) {
        char path[PCI_DEVFS_PATH_MAX];
        int pn = snprintf(path, sizeof(path), "%s/%s/%s",
                          PCI_DEVFS_NAMESPACE, bdf, s_attr_names[i]);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            log_warn("pci_devfs: path too long for attribute '%s'\n",
                     s_attr_names[i]);
            continue;
        }

        pci_attr_ctx_t *ctx = (pci_attr_ctx_t *)kmalloc(sizeof(*ctx));
        if (!ctx) {
            log_warn("pci_devfs: out of memory for '%s'\n", path);
            continue;
        }
        ctx->pdev = (struct pci_device *)pdev;
        ctx->attr = (uint8_t)i;

        int rc = devfs_register_path(path, ctx, &pci_attr_ops);
        if (rc == 0) {
            published++;
        } else if (rc == -EEXIST) {
            // Already published (idempotent re-registration): the node is
            // present but keeps the context it was created with, so ours
            // is dead weight and must not be leaked.
            kfree(ctx);
            published++;
        } else {
            log_warn("pci_devfs: register '%s' failed (%d)\n", path, rc);
            kfree(ctx);
        }
    }
    return published;
}

static bool s_pci_devfs_registered = false;
static int  s_pci_devfs_published = 0;

// Publish the whole bus.  Called once from the kernel boot path after
// devfs is mounted; a second call is a no-op that returns the count.
int pci_devfs_register_all(void)
{
    if (s_pci_devfs_registered) return s_pci_devfs_published;

    unsigned int count = pci_device_count();
    int total = 0;
    for (unsigned int i = 0; i < count; i++) {
        struct pci_device *pdev = pci_device_get(i);
        if (!pdev) continue;
        int r = pci_devfs_register_device(pdev);
        if (r > 0) total += r;
    }

    s_pci_devfs_published = total;
    s_pci_devfs_registered = true;
    log_info("pci_devfs: published %d attribute nodes for %u PCI devices\n",
             total, count);
    return total;
}
