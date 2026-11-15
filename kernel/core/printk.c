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

#if defined(__x86_64__) && !defined(OS01_HOST_TEST) && defined(FB_SHADOW) && FB_SHADOW
#include <memory/pmm.h>
#include <memory/memory.h>

static struct {
    struct Page *pages;
    uint64_t     n_pages;
    uint8_t     *virt;
    const void  *hw_fb;
    uint32_t     xres;
    uint32_t     yres;
    bool         failed;
} g_fb_shadow;

void printk_fb_shadow_release(void)
{
    if (g_fb_shadow.pages) {
        free_pages(g_fb_shadow.pages, (int32_t)g_fb_shadow.n_pages);
        g_fb_shadow.pages = NULL;
        g_fb_shadow.n_pages = 0;
        g_fb_shadow.virt = NULL;
    }
    g_fb_shadow.hw_fb = NULL;
    g_fb_shadow.xres = 0;
    g_fb_shadow.yres = 0;
    g_fb_shadow.failed = false;
}

uint8_t *printk_fb_shadow_get(const void *hw_fb, uint32_t xres, uint32_t yres)
{
    if (!hw_fb || xres == 0 || yres == 0)
        return NULL;
    if (g_fb_shadow.virt && g_fb_shadow.hw_fb == hw_fb &&
        g_fb_shadow.xres == xres && g_fb_shadow.yres == yres)
        return g_fb_shadow.virt;

    if (g_fb_shadow.virt)
        printk_fb_shadow_release();
    if (g_fb_shadow.failed)
        return NULL;
    struct pmm_stats st = {0};
    pmm_get_stats(&st);
    if (st.managed_bytes == 0 || st.free_bytes == 0)
        return NULL;

    size_t need_bytes = (size_t)xres * (size_t)yres * sizeof(uint32_t);
    uint64_t n_pages = (need_bytes + PAGE_2M_SIZE - 1) / PAGE_2M_SIZE;
    if (n_pages == 0 || st.free_bytes <= (n_pages + 8) * PAGE_2M_SIZE) {
        g_fb_shadow.failed = true;
        return NULL;
    }
    struct Page *p = alloc_pages(ZONE_NORMAL, n_pages, 0);
    if (!p) {
        g_fb_shadow.failed = true;
        return NULL;
    }

    uint8_t *virt = (uint8_t *)Phy_To_Virt(p->phy_address);
    memcpy(virt, hw_fb, need_bytes);
    g_fb_shadow.pages = p;
    g_fb_shadow.n_pages = n_pages;
    g_fb_shadow.virt = virt;
    g_fb_shadow.hw_fb = hw_fb;
    g_fb_shadow.xres = xres;
    g_fb_shadow.yres = yres;
    return virt;
}
#else
void printk_fb_shadow_release(void) {}
uint8_t *printk_fb_shadow_get(const void *hw_fb, uint32_t xres, uint32_t yres)
{
    (void)hw_fb; (void)xres; (void)yres;
    return NULL;
}
#endif

void printk_fb_shadow_scroll(void *hw_fb, uint32_t xres, uint32_t yres,
                             uint32_t row_bytes, int rows)
{
    if (!hw_fb || rows <= 0 || row_bytes == 0) return;
    size_t keep_bytes = (size_t)row_bytes * (size_t)(rows - 1);
    uint8_t *shadow = printk_fb_shadow_get(hw_fb, xres, yres);
    if (shadow) {
        if (keep_bytes > 0)
            memmove(shadow, shadow + row_bytes, keep_bytes);
        memset(shadow + keep_bytes, 0, row_bytes);
        memcpy(hw_fb, shadow, (size_t)row_bytes * (size_t)rows);
        return;
    }
    uint8_t *fb = (uint8_t *)hw_fb;
    if (keep_bytes > 0)
        memmove(fb, fb + row_bytes, keep_bytes);
    memset(fb + keep_bytes, 0, row_bytes);
}

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
    uint32_t yres = (snap && snap->state.info.height) ? snap->state.info.height : Pos.YResolution;
    uint32_t *shadow = (uint32_t *)printk_fb_shadow_get(fb_base, xres, yres);
    for (i = 0; i < font->height; i++)
    {
        uint32_t off = xres * (Pos.YPosition * font->height + i) + Pos.XPosition * font->width;
        addr = (uint32_t *)fb_base + off;
        uint32_t *saddr = shadow ? (shadow + off) : NULL;
        testval = 0x100;
        for (j = 0; j < font->width; j++)
        {
            testval = testval >> 1;
            uint32_t px = (*glyph & testval) ? FRcolor : BKcolor;
            *addr++ = px;
            if (saddr) *saddr++ = px;
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

    uint32_t *shadow = (uint32_t *)printk_fb_shadow_get(fb_base, xres, yres);
    for (i = 0; i < (int)font->height; i++) {
        uint32_t off = xres * (pixel_row_start + i) + pixel_col_start;
        addr = (uint32_t *)fb_base + off;
        uint32_t *saddr = shadow ? (shadow + off) : NULL;
        testval = 0x100;
        for (j = 0; j < (int)font->width; j++) {
            testval = testval >> 1;
            uint32_t px = (*glyph & testval) ? FRcolor : BKcolor;
            *addr++ = px;
            if (saddr) *saddr++ = px;
        }
        glyph++;
    }
}

void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    putchar_at_snap(NULL, col, row, FRcolor, BKcolor, c);
}

/* Render `len` bytes from `str` to the framebuffer under `Pos.lock`.
 * Caller MUST hold `Pos.lock`.  Returns 0 on success, or a negative
 * errno if no framebuffer writer lease is available (e.g. not yet
 * mapped, transitioning, or surrendered to userspace). */
static int color_write_fb_locked(unsigned int FRcolor, unsigned int BKcolor,
				 const char *str, int len)
{
	int count = 0;
	int line = 0;
	const struct fb_snapshot *snap = NULL;
#if PRINTK_FB_WRITER_LEASE
	fb_lease_t lease;
	int lrc = fb_writer_begin(&lease, 0);
	if (lrc < 0) {
		return lrc;
	}
	snap = &lease.snapshot;
#endif

	for(count = 0; count < len || line; count++)
	{
		if(line > 0)
		{
			count--;
			goto Label_tab;
		}
		if((unsigned char)*(str + count) == '\n')
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(str + count) == '\r')
		{
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(str + count) == '\b')
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
		else if((unsigned char)*(str + count) == '\t')
		{
			line = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;

Label_tab:
			line--;
			putchark_snap(snap, FRcolor , BKcolor , ' ');
			Pos.XPosition++;
		}
		else
		{
			putchark_snap(snap, FRcolor , BKcolor , (unsigned char)*(str + count));
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
			uint32_t row_bytes = pitch * font->height;
			uint8_t *fb = (uint8_t *)((snap && snap->addr) ? snap->addr : Pos.FB_addr);
			printk_fb_shadow_scroll(fb, cur_xres, cur_yres, row_bytes, rows);
			Pos.YPosition = rows - 1;
		}

	}
#if PRINTK_FB_WRITER_LEASE
	fb_writer_end(&lease);
#endif
	return 0;
}

int printk_fb_write(unsigned int FRcolor, unsigned int BKcolor,
		    const char *buf, int len)
{
	if (!buf || len <= 0)
		return 0;
	uint64_t flags = spin_lock_irqsave(&Pos.lock);
	int rc = color_write_fb_locked(FRcolor, BKcolor, buf, len);
	spin_unlock_irqrestore(&Pos.lock, flags);
	return rc;
}

int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...)
{
	int i = 0;
	int count = 0;
	va_list args;
	uint64_t flags = spin_lock_irqsave(&Pos.lock);

	va_start(args, fmt);
	i = vsprintf(buf_color, fmt, args);
	va_end(args);

	int rc = color_write_fb_locked(FRcolor, BKcolor, buf_color, i);
	spin_unlock_irqrestore(&Pos.lock, flags);

	if (rc < 0) {
		// No framebuffer writer lease (transitioning, not yet mapped,
		// or failed): continue on serial only, leaving the screen
		// cursor untouched.  Drop Pos.lock BEFORE taking serial_lock
		// to prevent AB-BA deadlock.
		uint64_t sf = spin_lock_irqsave(&serial_lock);
		for(count = 0; count < i; count++)
		{
			write_serial_unlocked((unsigned char)*(buf_color + count));
		}
		spin_unlock_irqrestore(&serial_lock, sf);
	}

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
