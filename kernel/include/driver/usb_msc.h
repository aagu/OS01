/* kernel/include/driver/usb_msc.h — Generic USB Mass Storage Class interface */
#ifndef _DRIVER_USB_MSC_H
#define _DRIVER_USB_MSC_H

#include <stddef.h>
#include <stdint.h>

struct usb_msc_transport {
    void *ctx;
    int (*control_no_data)(void *ctx, uint8_t request_type, uint8_t request,
                           uint16_t value, uint16_t index);
    int (*bulk)(void *ctx, uint8_t ep_addr, void *buf, size_t len, size_t *actual);
    int (*clear_halt)(void *ctx, uint8_t ep_addr);
};

#endif /* _DRIVER_USB_MSC_H */
