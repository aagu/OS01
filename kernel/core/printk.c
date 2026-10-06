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
#include <driver/fb_state.h>
#include <driver/font.h>
#include <core/printk.h>

/* The framebuffer writer-lease / snapshot subsystem (driver/fb_state.c,
 * fb_writer_begin/fb_writer_end) is built for x86_64 only — aarch64 keeps
 * PL011 serial as its console and maps the framebuffer directly through
 * frame_buffer_init().  When the lease subsystem is absent, color_printk
 * draws straight to the live Pos mapping with a NULL snapshot; the
 * snapshot helpers' NULL guard keeps that a no-op whenever no framebuffer
 * is mapped. */
#if defined(__x86_64__)
# define PRINTK_FB_WRITER_LEASE 1
#else
# define PRINTK_FB_WRITER_LEASE 0
#endif

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

/* Snapshot-aware glyph writer.  Draws one glyph at the current cursor
 * cell (Pos.XPosition/YPosition).  When `snap` is provided, its mapped
 * address and geometry are used instead of the Pos mirrors so a writer
 * holding a lease renders into a consistent framebuffer; NULL falls back
 * to Pos.FB_addr / Pos.XResolution.  NULL-safe: a missing framebuffer
 * makes the draw a no-op. */
void putchark_snap(const struct fb_snapshot *snap, unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    const uint32_t *fb_base = (snap && snap->addr) ? (const uint32_t *)snap->addr : Pos.FB_addr;
    if (!fb_base) return;
    uint32_t i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start + font->headersize +
            (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    uint32_t xres = (snap && snap->state.info.width) ? snap->state.info.width : Pos.XResolution;
    for (i = 0; i < font->height; i++)
    {
        addr = (uint32_t *)fb_base + xres * (Pos.YPosition * font->height + i) + Pos.XPosition * font->width;
        testval = 0x100;
        for (j = 0; j < font->width; j++)
        {
            testval = testval >> 1;
            if (*glyph & testval)
                *addr = FRcolor;
            else
                *addr = BKcolor;
            addr++;
        }
        glyph++;
    }
}

void putchark(unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    putchark_snap(NULL, FRcolor, BKcolor, c);
}

/* Snapshot-aware glyph writer at an explicit character-cell position.
 * Does NOT touch Pos.XPosition/YPosition or Pos.lock.  NULL-safe: a
 * missing framebuffer makes the draw a no-op. */
void putchar_at_snap(const struct fb_snapshot *snap, int col, int row,
                     unsigned int FRcolor, unsigned int BKcolor, unsigned char c)
{
    const uint32_t *fb_base = (snap && snap->addr) ? (const uint32_t *)snap->addr : Pos.FB_addr;
    if (!fb_base) return;
    int i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start
        + font->headersize
        + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    uint32_t xres = (snap && snap->state.info.width) ? snap->state.info.width : Pos.XResolution;
    uint32_t yres = (snap && snap->state.info.height) ? snap->state.info.height : Pos.YResolution;
    int pixel_row_start = row * (int)font->height;
    int pixel_col_start = col * (int)font->width;
    int max_rows = (int)(yres / font->height);
    int max_cols = (int)(xres / font->width);

    // Clamp to framebuffer bounds
    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;

    for (i = 0; i < (int)font->height; i++) {
        addr = (uint32_t *)fb_base + xres * (pixel_row_start + i) + pixel_col_start;
        testval = 0x100;
        for (j = 0; j < (int)font->width; j++) {
            testval = testval >> 1;
            if (*glyph & testval)
                *addr = FRcolor;
            else
                *addr = BKcolor;
            addr++;
        }
        glyph++;
    }
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    putchar_at_snap(NULL, col, row, FRcolor, BKcolor, c);
}

int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...)
{
	int i = 0;
	int count = 0;
	int line = 0;
	va_list args;
	uint64_t flags = spin_lock_irqsave(&Pos.lock);

	va_start(args, fmt);
	i = vsprintf(buf_color, fmt, args);
	va_end(args);

	const struct fb_snapshot *snap = NULL;
#if PRINTK_FB_WRITER_LEASE
	fb_lease_t lease;
	int lrc = fb_writer_begin(&lease, 0);
	if (lrc < 0) {
		// No framebuffer writer lease (transitioning, not yet mapped,
		// or failed): continue on serial only, leaving the screen
		// cursor untouched.  Drop Pos.lock BEFORE taking serial_lock
		// to prevent AB-BA deadlock.
		spin_unlock_irqrestore(&Pos.lock, flags);
		uint64_t sf = spin_lock_irqsave(&serial_lock);
		for(count = 0; count < i; count++)
		{
			write_serial_unlocked((unsigned char)*(buf_color + count));
		}
		spin_unlock_irqrestore(&serial_lock, sf);
		return i;
	}
	snap = &lease.snapshot;
#endif

	for(count = 0;count < i || line;count++)
	{
		if(line > 0)
		{
			count--;
			goto Label_tab;
		}
		if((unsigned char)*(buf_color + count) == '\n')
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(buf_color + count) == '\r')
		{
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(buf_color + count) == '\b')
		{
			Pos.XPosition--;
			if(Pos.XPosition < 0)
			{
				Pos.XPosition = (Pos.XResolution / font->width - 1) * font->width;
				Pos.YPosition--;
				if(Pos.YPosition < 0)
					Pos.YPosition = (Pos.YResolution / font->height - 1) * font->height;
			}
			putchark_snap(snap, FRcolor , BKcolor , ' ');
		}
		else if((unsigned char)*(buf_color + count) == '\t')
		{
			line = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;

Label_tab:
			line--;
			putchark_snap(snap, FRcolor , BKcolor , ' ');
			Pos.XPosition++;
		}
		else
		{
			putchark_snap(snap, FRcolor , BKcolor , (unsigned char)*(buf_color + count));
			Pos.XPosition++;
		}


		uint32_t cur_xres = (snap && snap->state.info.width) ? snap->state.info.width : Pos.XResolution;
		uint32_t cur_yres = (snap && snap->state.info.height) ? snap->state.info.height : Pos.YResolution;
		if(Pos.XPosition >= (int32_t)(cur_xres / font->width))
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		if(Pos.YPosition >= (int32_t)(cur_yres / font->height))
		{
			// Scroll framebuffer up by one character row.
			int rows = (int)(cur_yres / font->height);
			uint32_t pitch = cur_xres * sizeof(uint32_t);
			int row_bytes = (int)(pitch * font->height);
			uint8_t *fb = (uint8_t *)((snap && snap->addr) ? snap->addr : Pos.FB_addr);
			if (fb) {
				memmove(fb, fb + row_bytes, (uintptr_t)row_bytes * (rows - 1));
				memset(fb + (uintptr_t)row_bytes * (rows - 1), 0, (uintptr_t)row_bytes);
			}
			Pos.YPosition = rows - 1;
		}

	}
#if PRINTK_FB_WRITER_LEASE
	fb_writer_end(&lease);
#endif
	spin_unlock_irqrestore(&Pos.lock, flags);

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
