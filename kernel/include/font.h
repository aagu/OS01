#ifndef _KERNEL_FONT_H
#define _KERNEL_FONT_H

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
    uint8_t glyphs;
} __attribute__((packed)) psf2_t;

extern volatile unsigned char _binary_kernel_font_psf_start;

#endif