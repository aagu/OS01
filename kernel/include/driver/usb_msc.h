/* kernel/include/driver/usb_msc.h — Generic USB Mass Storage Class interface */
#ifndef _DRIVER_USB_MSC_H
#define _DRIVER_USB_MSC_H

#include <stddef.h>
#include <stdint.h>
#include <block/blockdev.h>

struct usb_msc_transport {
    void *ctx;
    int (*control_no_data)(void *ctx, uint8_t request_type, uint8_t request,
                           uint16_t value, uint16_t index);
    int (*bulk)(void *ctx, uint8_t ep_addr, void *buf, size_t len, size_t *actual);
    int (*clear_halt)(void *ctx, uint8_t ep_addr);
};

int usb_msc_probe(const struct usb_msc_transport *transport,
                  uint8_t interface_num, uint8_t in_addr, uint8_t out_addr,
                  const char *name, block_device_t **out);

#endif /* _DRIVER_USB_MSC_H */
