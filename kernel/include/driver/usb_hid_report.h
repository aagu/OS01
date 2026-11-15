#ifndef _DRIVER_USB_HID_REPORT_H
#define _DRIVER_USB_HID_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include <driver/input_ps2.h>
#include <uapi/mouse.h>

#define USB_HID_MAX_FRAMES 20

typedef struct {
    uint8_t modifiers;      /* bitmask of currently held modifiers (bits 0..7) */
    uint8_t keys[6];        /* sorted array of currently held valid HID usages */
    uint8_t key_count;      /* number of valid usages in keys (0..6) */
} usb_hid_keyboard_t;

int usb_hid_keyboard_update(usb_hid_keyboard_t *state, const uint8_t *report,
                            size_t len, input_ps2_frame_t out[USB_HID_MAX_FRAMES]);

size_t usb_hid_keyboard_release(usb_hid_keyboard_t *state,
                               input_ps2_frame_t out[USB_HID_MAX_FRAMES]);

int usb_hid_mouse_decode(const uint8_t *report, size_t len, uint16_t mps,
                         mouse_event_t *out);

#endif /* _DRIVER_USB_HID_REPORT_H */
