// kernel/core/printk.c — kernel-side console shared by x86_64 and aarch64
// (spec 2026-10-06).
//
// Both arches link this TU: x86_64 via the `core/*.c` wildcard in
// kernel/Makefile, aarch64 via the explicit addition to its KERNEL_C_SOURCES
// list. The colour drawing logic (putchark / putchar_at / color_printk)
// and the canonical PSF font reference live HERE — single source of
// truth, no per-arch duplication. Per-arch MMIO mapping is in
// kernel/arch/<arch>/runtime/printk_fb.c.
//
// vsprintf comes from -D__is_libk via stdio/vsprintf.c (in libk.a on
// both arches). serial_printk uses <driver/serial.h>'s write_serial_
// unlocked + serial_lock; both arches supply those (x86_64 via
// kernel/driver/serial.c, aarch64 via kernel/arch/aarch64/platform/serial.c).

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <arch/spinlock.h>
#include <driver/serial.h>
#include <driver/font.h>
#include <core/printk.h>

/* Font symbols — produced by the kernel/Makefile `ld -r -b binary`
 * recipe on driver/font.psf for both arches. */
extern volatile unsigned char _binary_kernel_font_psf_start;
extern volatile unsigned char _binary_kernel_font_psf_end;

psf2_t *font = (psf2_t *)&_binary_kernel_font_psf_start;

/* Canonical Pos — single global across the whole kernel. aarch64
 * boot_fb_init() / x86_64_boot_early() each populate the geometry and
 * call spin_init(&Pos.lock). */
position Pos;

/* Separate per-function buffers — color_printk and serial_printk must
 * not share a buffer because int $0x80 / IRQ handlers can fire between
 * vsprintf and the putchark / write_serial_unlocked loops in either
 * function. */
static char buf_color[4096];
static char buf_serial[4096];

void putchark(unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    /* NULL-safe: when no framebuffer is configured (boot path without
     * ramfb, or early-boot before Pos is populated), skip the pixel
     * write. spin_lock in color_printk still serialises per-char. */
    if (!Pos.FB_addr) return;

    uint32_t i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    const unsigned char *glyph =
        (const unsigned char *)&_binary_kernel_font_psf_start
        + font->headersize
        + ((c > 0 && c < font->numglyph) ? c : 0) * font->bytesperglyph;

    for (i = 0; i < font->height; i++) {
        addr = Pos.FB_addr
             + Pos.XResolution * (Pos.YPosition * font->height + i)
             + Pos.XPosition * font->width;
        testval = 0x100;
        for (j = 0; j < font->width; j++) {
            testval = testval >> 1;
            *addr = (*glyph & testval) ? FRcolor : BKcolor;
            addr++;
        }
        glyph++;
    }
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    if (!Pos.FB_addr) return;

    int i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    const unsigned char *glyph =
        (const unsigned char *)&_binary_kernel_font_psf_start
        + font->headersize
        + ((c > 0 && c < font->numglyph) ? c : 0) * font->bytesperglyph;

    int pixel_row_start = row * (int)font->height;
    int pixel_col_start = col * (int)font->width;
    int max_rows = (int)(Pos.YResolution / font->height);
    int max_cols = (int)(Pos.XResolution / font->width);

    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;

    for (i = 0; i < (int)font->height; i++) {
        addr = Pos.FB_addr
             + Pos.XResolution * (pixel_row_start + i)
             + pixel_col_start;
        testval = 0x100;
        for (j = 0; j < font->width; j++) {
            testval = testval >> 1;
            *addr = (*glyph & testval) ? FRcolor : BKcolor;
            addr++;
        }
        glyph++;
    }
}

int color_printk(unsigned int FRcolor, unsigned int BKcolor, const char *fmt, ...)
{
    /* Skip the draw path entirely when no framebuffer is mapped. Avoids
     * the scroll-trigger NULL deref and avoids contention on Pos.lock
     * if a caller hits it before spin_init(&Pos.lock) has run. */
    if (!Pos.FB_addr) return 0;

    int i = 0;
    int count = 0;
    int line = 0;
    va_list args;

    spin_lock(&Pos.lock);

    va_start(args, fmt);
    i = vsprintf(buf_color, fmt, args);
    va_end(args);

    for (count = 0; count < i || line; count++) {
        if (line > 0) {
            count--;
            goto Label_tab;
        }
        if ((unsigned char)buf_color[count] == '\n') {
            Pos.YPosition++;
            Pos.XPosition = 0;
        } else if ((unsigned char)buf_color[count] == '\r') {
            Pos.XPosition = 0;
        } else if ((unsigned char)buf_color[count] == '\b') {
            Pos.XPosition--;
            if (Pos.XPosition < 0) {
                Pos.XPosition =
                    (Pos.XResolution / font->width - 1) * font->width;
                Pos.YPosition--;
                if (Pos.YPosition < 0)
                    Pos.YPosition =
                        (Pos.YResolution / font->height - 1) * font->height;
            }
            putchark(FRcolor, BKcolor, ' ');
        } else if ((unsigned char)buf_color[count] == '\t') {
            line = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;
        Label_tab:
            line--;
            putchark(FRcolor, BKcolor, ' ');
            Pos.XPosition++;
        } else {
            putchark(FRcolor, BKcolor, (unsigned char)buf_color[count]);
            Pos.XPosition++;
        }

        if (Pos.XPosition >= (int32_t)(Pos.XResolution / font->width)) {
            Pos.YPosition++;
            Pos.XPosition = 0;
        }
        if (Pos.YPosition >= (int32_t)(Pos.YResolution / font->height)) {
            int rows = (int)(Pos.YResolution / font->height);
            uint32_t pitch = Pos.XResolution * sizeof(uint32_t);
            int row_bytes = (int)(pitch * font->height);
            uint8_t *fb = (uint8_t *)Pos.FB_addr;
            memmove(fb, fb + row_bytes, (uintptr_t)row_bytes * (rows - 1));
            memset(fb + (uintptr_t)row_bytes * (rows - 1), 0,
                   (uintptr_t)row_bytes);
            Pos.YPosition = rows - 1;
        }
    }

    spin_unlock(&Pos.lock);
    return i;
}

int serial_printk(const char *fmt, ...)
{
    int i = 0;
    int count = 0;
    uint64_t sf = spin_lock_irqsave(&serial_lock);

    va_list args;
    va_start(args, fmt);
    i = vsprintf(buf_serial, fmt, args);
    va_end(args);

    for (count = 0; count < i; count++) {
        write_serial_unlocked((unsigned char)buf_serial[count]);
    }

    spin_unlock_irqrestore(&serial_lock, sf);
    return i;
}