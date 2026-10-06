#ifndef _UAPI_FB_H
#define _UAPI_FB_H

#include <stdint.h>
#include <stddef.h>

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define FB_MAX_MODES 16
#define FB_FORMAT_RGB32 0 /* XRGB8888: 数值0x00RRGGBB，小端字节B,G,R,X */

struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed)); /* 20 bytes，旧read ABI不增加字段 */

struct fb_modes_req {
    uint32_t capacity, count, total;
    struct fb_info modes[FB_MAX_MODES];
}; /* 332 bytes，alignment=4 */

struct fb_set_mode_req {
    uint32_t width, height, bpp;
}; /* 12 bytes；bpp=0或32 */

struct fb_state {
    struct fb_info info;
    uint32_t reserved; /* 必须为0 */
    uint64_t generation;
}; /* 32 bytes，generation offset=24 */

#define FBIOSURRENDER     0x00004601
#define FBIOGET_MODES     0x00004602
#define FBIOSET_MODE      0x00004603
#define FBIOGET_CURR_MODE 0x00004604
#define FBIOGET_STATE     0x00004605

_Static_assert(sizeof(struct fb_info) == 20, "fb_info must be 20 bytes (ABI)");
_Static_assert(sizeof(struct fb_modes_req) == 332, "fb_modes_req must be 332 bytes (ABI)");
_Static_assert(_Alignof(struct fb_modes_req) == 4, "fb_modes_req must have alignment 4 (ABI)");
_Static_assert(sizeof(struct fb_set_mode_req) == 12, "fb_set_mode_req must be 12 bytes (ABI)");
_Static_assert(sizeof(struct fb_state) == 32, "fb_state must be 32 bytes (ABI)");
_Static_assert(offsetof(struct fb_state, generation) == 24, "fb_state generation offset must be 24 (ABI)");

#endif /* _UAPI_FB_H */
