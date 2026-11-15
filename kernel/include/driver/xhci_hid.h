/* kernel/include/driver/xhci_hid.h — xHCI Boot HID interface selection (kernel-private) */
#ifndef _KERNEL_DRIVER_XHCI_HID_H
#define _KERNEL_DRIVER_XHCI_HID_H

#include <stdint.h>
#include <stdbool.h>

#define USB_CLASS_HID           0x03U
#define USB_HID_SUBCLASS_BOOT   0x01U
#define USB_HID_PROTO_KEYBOARD  0x01U
#define USB_HID_PROTO_MOUSE     0x02U

enum xhci_hid_kind {
    XHCI_HID_KEYBOARD = 0,
    XHCI_HID_MOUSE = 1,
    XHCI_HID_KINDS
};

struct xhci_hid_candidate {
    bool valid;
    uint8_t iface;
    uint8_t dci;
    uint8_t ep_addr;
    uint8_t interval;
    uint16_t mps;
};

int xhci_hid_scan_config(const uint8_t *cfg, uint16_t len, uint8_t speed_class,
                         struct xhci_hid_candidate out[XHCI_HID_KINDS],
                         uint32_t *hid_skip_dcis);

#endif /* _KERNEL_DRIVER_XHCI_HID_H */
