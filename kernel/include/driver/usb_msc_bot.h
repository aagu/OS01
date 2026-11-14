/* kernel/include/driver/usb_msc_bot.h — USB Mass Storage Bulk-Only Transport */
#ifndef _DRIVER_USB_MSC_BOT_H
#define _DRIVER_USB_MSC_BOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <driver/usb_msc.h>

struct usb_msc_bot {
    struct usb_msc_transport transport;
    uint8_t interface_num;
    uint8_t in_addr;
    uint8_t out_addr;
    uint32_t next_tag;
    bool dead;
};

enum usb_msc_dir {
    USB_MSC_NONE = 0,
    USB_MSC_IN = 1,
    USB_MSC_OUT = 2,
};

int usb_msc_bot_command(struct usb_msc_bot *b, const uint8_t *cdb, uint8_t cdb_len,
                        enum usb_msc_dir dir, void *buf, uint32_t len,
                        bool retry_safe, size_t *actual);

#endif /* _DRIVER_USB_MSC_BOT_H */
