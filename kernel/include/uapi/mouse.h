#ifndef _UAPI_MOUSE_H
#define _UAPI_MOUSE_H

#include <stdint.h>

/* /dev/mouse — normalized mouse event stream.  Events are a fixed
 * 8 bytes regardless of the hardware 3/4-byte packet mode; must match
 * userspace ABI exactly. */
typedef struct {
    uint8_t buttons;   /* bit0=左 bit1=右 bit2=中，其余为 0 */
    int8_t wheel;      /* 基础鼠标恒为 0 */
    int16_t dx;        /* 相对位移，正值向右 */
    int16_t dy;        /* 相对位移，正值向下 */
    uint16_t reserved; /* 写 0，用户态忽略 */
} mouse_event_t;

_Static_assert(sizeof(mouse_event_t) == 8,
               "mouse_event_t must be a fixed 8-byte UAPI");

#endif
