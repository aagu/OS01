#ifndef _KERNEL_FB_H
#define _KERNEL_FB_H

#include <stdint.h>

struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601

#endif
