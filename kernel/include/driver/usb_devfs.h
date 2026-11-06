/* kernel/include/driver/usb_devfs.h — USB devfs node interface */
#ifndef _DRIVER_USB_DEVFS_H
#define _DRIVER_USB_DEVFS_H

#include <stdint.h>
#include <stddef.h>
#include <driver/xhci.h>

#define USB_DEVFS_NAMESPACE "usb"
#define USB_DEVFS_ID_MAX    32
#define USB_DEVFS_PATH_MAX  64
#define USB_DEVFS_FMT_MAX   128

typedef enum {
    USB_ATTR_BUS = 0,
    USB_ATTR_DEVNUM,
    USB_ATTR_PORT,
    USB_ATTR_VENDOR,
    USB_ATTR_PRODUCT,
    USB_ATTR_CLASS,
    USB_ATTR_SUBCLASS,
    USB_ATTR_PROTOCOL,
    USB_ATTR_SPEED,
    USB_ATTR_DESCRIPTORS,
    USB_ATTR_COUNT
} usb_attr_id_t;

typedef struct {
    struct xhci_slot *slot;
    uint8_t bus_id;
    uint8_t attr;
} usb_attr_ctx_t;

struct vfs_node;

int usb_devfs_format_attr(const struct xhci_slot *slot, uint8_t bus_id,
                           usb_attr_id_t attr, char *buf, size_t size);
int usb_devfs_register_slot(uint8_t bus_id, struct xhci_slot *slot);
int usb_devfs_register_all(void);
int usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);

#endif /* _DRIVER_USB_DEVFS_H */
