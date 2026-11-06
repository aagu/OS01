/* kernel/driver/usb_devfs.c — USB devfs node registration and attributes */
#include <driver/usb_devfs.h>
#include <driver/xhci.h>
#include <fs/devfs.h>
#include <memory/slab.h>
#include <log/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *s_attr_names[USB_ATTR_COUNT] = {
    [USB_ATTR_BUS]         = "bus",
    [USB_ATTR_DEVNUM]      = "devnum",
    [USB_ATTR_PORT]        = "port",
    [USB_ATTR_VENDOR]      = "vendor",
    [USB_ATTR_PRODUCT]     = "product",
    [USB_ATTR_CLASS]       = "class",
    [USB_ATTR_SUBCLASS]    = "subclass",
    [USB_ATTR_PROTOCOL]    = "protocol",
    [USB_ATTR_SPEED]       = "speed",
    [USB_ATTR_DESCRIPTORS] = "descriptors",
};

int usb_devfs_format_attr(const struct xhci_slot *slot, uint8_t bus_id,
                           usb_attr_id_t attr, char *buf, size_t size)
{
    if (!slot || attr >= USB_ATTR_COUNT) return -EINVAL;

    switch (attr) {
    case USB_ATTR_BUS:
        return snprintf(buf, size, "%u\n", (unsigned int)bus_id);
    case USB_ATTR_DEVNUM:
        return snprintf(buf, size, "%u\n", (unsigned int)slot->slot_id);
    case USB_ATTR_PORT:
        return snprintf(buf, size, "%u\n", (unsigned int)slot->port_num);
    case USB_ATTR_VENDOR:
        return snprintf(buf, size, "0x%04x\n", (unsigned int)slot->desc.idVendor);
    case USB_ATTR_PRODUCT:
        return snprintf(buf, size, "0x%04x\n", (unsigned int)slot->desc.idProduct);
    case USB_ATTR_CLASS:
        return snprintf(buf, size, "0x%02x\n", (unsigned int)slot->desc.bDeviceClass);
    case USB_ATTR_SUBCLASS:
        return snprintf(buf, size, "0x%02x\n", (unsigned int)slot->desc.bDeviceSubClass);
    case USB_ATTR_PROTOCOL:
        return snprintf(buf, size, "0x%02x\n", (unsigned int)slot->desc.bDeviceProtocol);
    case USB_ATTR_SPEED: {
        const char *s = "unknown";
        switch (slot->speed) {
        case XHCI_PORT_SPEED_FULL:  s = "full"; break;
        case XHCI_PORT_SPEED_LOW:   s = "low"; break;
        case XHCI_PORT_SPEED_HIGH:  s = "high"; break;
        case XHCI_PORT_SPEED_SUPER: s = "super"; break;
        default:                    s = "unknown"; break;
        }
        return snprintf(buf, size, "%s\n", s);
    }
    case USB_ATTR_DESCRIPTORS:
        if (!slot->raw_descriptors || slot->raw_descriptors_len == 0) return 0;
        if (buf && size > 0) {
            size_t n = (size < slot->raw_descriptors_len) ? size : slot->raw_descriptors_len;
            memcpy(buf, slot->raw_descriptors, n);
        }
        return (int)slot->raw_descriptors_len;
    default:
        return -EINVAL;
    }
}

int usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !buffer) return -EINVAL;
    if (size == 0) return 0;

    usb_attr_ctx_t *ctx = (usb_attr_ctx_t *)devfs_get_private(node);
    if (!ctx || !ctx->slot) return -EINVAL;

    if (ctx->attr == USB_ATTR_DESCRIPTORS) {
        if (!ctx->slot->raw_descriptors || ctx->slot->raw_descriptors_len == 0) return 0;
        if (offset >= (uint64_t)ctx->slot->raw_descriptors_len) return 0;

        uint64_t avail = (uint64_t)ctx->slot->raw_descriptors_len - offset;
        uint64_t n = (size < avail) ? size : avail;
        memcpy(buffer, ctx->slot->raw_descriptors + offset, n);
        return (int)n;
    }

    char tmp[USB_DEVFS_FMT_MAX];
    int len = usb_devfs_format_attr(ctx->slot, ctx->bus_id, (usb_attr_id_t)ctx->attr, tmp, sizeof(tmp));
    if (len < 0) return len;
    if (offset >= (uint64_t)len) return 0;

    uint64_t avail = (uint64_t)len - offset;
    uint64_t n = (size < avail) ? size : avail;
    memcpy(buffer, tmp + offset, n);
    return (int)n;
}

static const struct devfs_ops s_usb_attr_ops = {
    .read = usb_attr_read,
};

int usb_devfs_register_slot(uint8_t bus_id, struct xhci_slot *slot)
{
    if (!slot) return -EINVAL;

    int published = 0;
    for (int a = 0; a < USB_ATTR_COUNT; a++) {
        char path[USB_DEVFS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%u-%u/%s",
                 USB_DEVFS_NAMESPACE,
                 (unsigned int)bus_id,
                 (unsigned int)slot->port_num,
                 s_attr_names[a]);

        usb_attr_ctx_t *ctx = kmalloc(sizeof(usb_attr_ctx_t));
        if (!ctx) continue;
        ctx->slot = slot;
        ctx->bus_id = bus_id;
        ctx->attr = (uint8_t)a;

        int rc = devfs_register_path(path, ctx, &s_usb_attr_ops);
        if (rc == 0) {
            published++;
        } else if (rc == -EEXIST) {
            kfree(ctx);
            published++;
        } else {
            log_warn("[usb_devfs] failed to register %s: %d\n", path, rc);
            kfree(ctx);
        }
    }
    return published;
}

static bool s_usb_devfs_registered = false;
static int s_usb_devfs_published = 0;

int usb_devfs_register_all(void)
{
    if (s_usb_devfs_registered) return s_usb_devfs_published;

    uint8_t count = xhci_controller_count();
    int registered = 0;

    for (uint8_t b = 0; b < count; b++) {
        struct xhci_controller *ctrl = xhci_controller_get(b);
        if (!ctrl) continue;

        uint8_t bus_id = b + 1;
        for (uint8_t s = 1; s <= XHCI_MAX_MANAGED_SLOTS; s++) {
            struct xhci_slot *slot = ctrl->slots[s];
            if (slot && slot->active && slot->configured) {
                if (usb_devfs_register_slot(bus_id, slot) > 0) {
                    registered++;
                }
            }
        }
    }

    s_usb_devfs_registered = true;
    s_usb_devfs_published = registered;
    log_info("[usb_devfs] registered %d USB device(s)\n", registered);
    return registered;
}
