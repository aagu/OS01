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
//
// Geometry (2026-11-15): every framebuffer writer resolves ONE view
// (width, height, real byte pitch, writable range) and derives all its
// offsets from it.  A real panel is 1024x768 with a 4096-byte pitch,
// so the byte stride is LARGER than width*4 — the historic
// `xres * pixel_index` addressing was silently wrong on such a
// machine.  The x86 console also writes through the wide 32-bit store
// (kernel/include/arch/x86_64/fb_copy.h).

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <arch/spinlock.h>
#include <arch/clocksource.h>
#include <arch/fb_copy.h>     // fb_copy_rows / fb_scroll_rows: the geometry
#include <driver/serial.h>
#include <driver/fb_state.h>
#include <driver/font.h>
#include <core/printk.h>

/* The boot-log shadow needs the PMM, so it exists only in the x86
 * FB_SHADOW build.  Everything else degrades to the direct scroll. */
#if defined(__x86_64__) && !defined(OS01_HOST_TEST) && defined(FB_SHADOW) && FB_SHADOW
#  include <memory/pmm.h>
#  include <memory/memory.h>
#  define PRINTK_FB_SHADOW 1
#else
#  define PRINTK_FB_SHADOW 0
#endif

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

/* ── Framebuffer view resolution ─────────────────────────────── */

bool printk_fb_view_resolve(const struct fb_snapshot *snap, printk_fb_view_t *out)
{
    if (!out) return false;

    uint8_t *fb;
    uint32_t width, height, stride;
    uint64_t fb_bytes;

    if (snap && snap->addr) {
        fb       = (uint8_t *)snap->addr;
        width    = snap->state.info.width;
        height   = snap->state.info.height;
        stride   = snap->state.info.stride;
        fb_bytes = snap->mapped_size;
    } else {
        /* The legacy path (aarch64, and the pre-lease x86 draws): the
         * Pos mirrors are the only description of the framebuffer, and
         * its size is not published there — the range stays
         * unbounded. */
        if (Pos.XResolution <= 0 || Pos.YResolution <= 0) return false;
        fb       = (uint8_t *)Pos.FB_addr;
        width    = (uint32_t)Pos.XResolution;
        height   = (uint32_t)Pos.YResolution;
        stride   = width * 4u;
        fb_bytes = 0;
    }

    if (!fb || width == 0 || height == 0) return false;
    if (stride < width * 4u) stride = width * 4u;

    /* A framebuffer whose last row does not fit in the mapping is
     * DEGRADED, not refused: the console keeps working on the rows it
     * can reach instead of going dark. */
    if (fb_bytes != 0) {
        uint64_t max_rows = fb_bytes / (uint64_t)stride;
        if (max_rows < height) height = (uint32_t)max_rows;
    }
    if (height == 0) return false;

    out->fb       = fb;
    out->width    = width;
    out->height   = height;
    out->stride   = stride;
    out->fb_bytes = fb_bytes;
    return true;
}

/* ── The boot-log shadow (x86 + FB_SHADOW only) ────────────────
 *
 * The shadow holds the VISIBLE pixels of the framebuffer in a PACKED
 * row layout (row stride = width*4).  The framebuffer itself is
 * always addressed by its real pitch, so the shadow and the panel can
 * never disagree about the geometry.
 *
 * The identity of the shadow is (mapping address, width, height,
 * pitch, writable range): a mapping that changed — or the SAME
 * address with a DIFFERENT pitch — must rebuild the shadow, because
 * the old one describes a framebuffer that no longer exists.
 */
#if PRINTK_FB_SHADOW

static struct {
    struct Page *pages;
    uint64_t     n_pages;
    uint8_t     *virt;
    const void  *hw_fb;        /* mapping identity */
    uint32_t     width;
    uint32_t     height;
    uint32_t     stride;       /* the framebuffer byte pitch */
    uint64_t     fb_bytes;     /* the writable range */
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
    g_fb_shadow.width = 0;
    g_fb_shadow.height = 0;
    g_fb_shadow.stride = 0;
    g_fb_shadow.fb_bytes = 0;
    g_fb_shadow.failed = false;
}

/* Record a failed attempt together with the geometry it failed for, so
 * a LATER geometry (a mode switch) gets a fresh chance while the same
 * broken geometry is not retried on every glyph. */
static void printk_fb_shadow_mark_failed(const printk_fb_view_t *view)
{
    g_fb_shadow.failed    = true;
    g_fb_shadow.hw_fb     = (const void *)view->fb;
    g_fb_shadow.width     = view->width;
    g_fb_shadow.height    = view->height;
    g_fb_shadow.stride    = view->stride;
    g_fb_shadow.fb_bytes  = view->fb_bytes;
}

uint8_t *printk_fb_shadow_get(const printk_fb_view_t *view)
{
    if (!view || !printk_fb_view_ok(view)) return NULL;

    if (g_fb_shadow.virt) {
        if (g_fb_shadow.hw_fb == (const void *)view->fb &&
            g_fb_shadow.width == view->width &&
            g_fb_shadow.height == view->height &&
            g_fb_shadow.stride == view->stride &&
            g_fb_shadow.fb_bytes == view->fb_bytes)
            return g_fb_shadow.virt;

        /* The mapping, the pitch or the size changed: rebuild. */
        printk_fb_shadow_release();
    }

    if (g_fb_shadow.failed) {
        if (g_fb_shadow.hw_fb == (const void *)view->fb &&
            g_fb_shadow.width == view->width &&
            g_fb_shadow.height == view->height &&
            g_fb_shadow.stride == view->stride &&
            g_fb_shadow.fb_bytes == view->fb_bytes)
            return NULL;                 /* the same geometry failed before */

        /* A different geometry gets a new chance. */
        g_fb_shadow.failed = false;
    }

    struct pmm_stats st = {0};
    pmm_get_stats(&st);
    if (st.managed_bytes == 0 || st.free_bytes == 0)
        return NULL;                 /* the PMM is not ready yet */

    size_t row_bytes = (size_t)view->width * 4u;
    size_t need_bytes = row_bytes * (size_t)view->height;
    uint64_t n_pages = (need_bytes + PAGE_2M_SIZE - 1) / PAGE_2M_SIZE;
    if (n_pages == 0 || st.free_bytes <= (n_pages + 8) * PAGE_2M_SIZE) {
        printk_fb_shadow_mark_failed(view);
        return NULL;
    }
    struct Page *p = alloc_pages(ZONE_NORMAL, n_pages, 0);
    if (!p) {
        printk_fb_shadow_mark_failed(view);
        return NULL;
    }

    uint8_t *virt = (uint8_t *)Phy_To_Virt(p->phy_address);

    /* Seed the packed shadow from the framebuffer, row by row: the
     * real pitch is honoured and the padding is never mirrored.  The
     * visible pixels of the boot screen survive the first scroll. */
    for (uint32_t y = 0; y < view->height; y++) {
        uint64_t off;
        if (!printk_fb_row_offset(view, y, &off)) {
            free_pages(p, (int32_t)n_pages);
            printk_fb_shadow_mark_failed(view);
            return NULL;
        }
        memcpy(virt + (size_t)y * row_bytes, view->fb + off, row_bytes);
    }

    g_fb_shadow.pages    = p;
    g_fb_shadow.n_pages  = n_pages;
    g_fb_shadow.virt     = virt;
    g_fb_shadow.hw_fb    = (const void *)view->fb;
    g_fb_shadow.width    = view->width;
    g_fb_shadow.height   = view->height;
    g_fb_shadow.stride   = view->stride;
    g_fb_shadow.fb_bytes = view->fb_bytes;
    return virt;
}

#else  /* !PRINTK_FB_SHADOW */

void printk_fb_shadow_release(void) {}

uint8_t *printk_fb_shadow_get(const printk_fb_view_t *view)
{
    (void)view;
    return NULL;
}

#endif /* PRINTK_FB_SHADOW */

/* ── The scroll ──────────────────────────────────────────────── */

/* Scroll the visible framebuffer up by `shift_rows` VISIBLE pixel
 * rows.  Returns the time spent in the framebuffer write phase, or 0
 * when the geometry was rejected (in which case NOT a single pixel
 * was touched). */
uint64_t printk_fb_scroll_view(const printk_fb_view_t *view, uint32_t shift_rows)
{
    if (!view || !printk_fb_view_ok(view)) return 0;
    if (shift_rows == 0 || shift_rows >= view->height) return 0;

    uint32_t row_bytes = view->width * 4u;

    /* The shadow path: scroll inside the packed RAM shadow and write
     * the visible rows back to the framebuffer with the wide write.
     * The framebuffer is addressed by its REAL pitch and the padding
     * is never touched.
     *
     * The RAM side also uses wide copies of disjoint rows. */
    uint8_t *shadow = printk_fb_shadow_get(view);
    if (shadow) {
        fb_scroll_packed(shadow, row_bytes, view->height, shift_rows);

        uint64_t t0 = arch_clocksource_read_ns();
        bool ok = fb_copy_rows(view, shadow, row_bytes, view->height);
        uint64_t t1 = arch_clocksource_read_ns();
        return ok ? (t1 - t0) : 0;
    }

    /* No shadow (the PMM is not ready, the allocation failed, or the
     * build has no shadow at all): scroll the visible pixels in place,
     * row by row, using the real pitch. */
    uint64_t t0 = arch_clocksource_read_ns();
    bool ok = fb_scroll_rows(view, shift_rows);
    uint64_t t1 = arch_clocksource_read_ns();
    return ok ? (t1 - t0) : 0;
}

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
    printk_fb_view_t view;
    if (!printk_fb_view_resolve(snap, &view)) return;
    if (Pos.XPosition < 0 || Pos.YPosition < 0) return;

    uint32_t i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start + font->headersize +
            (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    uint32_t row_bytes = view.width * 4u;
    uint32_t pixel_row = (uint32_t)Pos.YPosition * font->height;
    uint32_t pixel_col = (uint32_t)Pos.XPosition * font->width;

    uint32_t *shadow = (uint32_t *)printk_fb_shadow_get(&view);
    for (i = 0; i < font->height; i++)
    {
        uint64_t off;
        if (!printk_fb_row_offset(&view, pixel_row + i, &off)) return;
        addr = (uint32_t *)(view.fb + off) + pixel_col;
        uint32_t *saddr = shadow ? (shadow + (size_t)(pixel_row + i) * row_bytes / 4 + pixel_col) : NULL;
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
    printk_fb_view_t view;
    if (!printk_fb_view_resolve(snap, &view)) return;

    int i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start
        + font->headersize
        + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    uint32_t row_bytes = view.width * 4u;
    int pixel_row_start = row * (int)font->height;
    int pixel_col_start = col * (int)font->width;
    int max_rows = (int)(view.height / font->height);
    int max_cols = (int)(view.width / font->width);

    // Clamp to framebuffer bounds
    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;

    uint32_t *shadow = (uint32_t *)printk_fb_shadow_get(&view);
    for (i = 0; i < (int)font->height; i++) {
        uint64_t off;
        if (!printk_fb_row_offset(&view, (uint32_t)(pixel_row_start + i), &off)) return;
        addr = (uint32_t *)(view.fb + off) + pixel_col_start;
        uint32_t *saddr = shadow ? (shadow + (size_t)(pixel_row_start + i) * row_bytes / 4 + pixel_col_start) : NULL;
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

	/* ONE geometry for the whole line: the glyphs, the cursor advance
	 * and the scroll all address the framebuffer the same way. */
	printk_fb_view_t view;
	bool have_view = printk_fb_view_resolve(snap, &view);
	uint32_t cur_xres = have_view ? view.width
	                              : (uint32_t)(Pos.XResolution > 0 ? Pos.XResolution : 0);
	uint32_t cur_yres = have_view ? view.height
	                              : (uint32_t)(Pos.YResolution > 0 ? Pos.YResolution : 0);

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


		if(Pos.XPosition >= (int32_t)(cur_xres / font->width))
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		if(Pos.YPosition >= (int32_t)(cur_yres / font->height))
		{
			// Scroll framebuffer up by one character row.
			int rows = (int)(cur_yres / font->height);
			if (have_view && rows > 0)
				(void)printk_fb_scroll_view(&view, font->height);
			if (rows > 0)
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

/* ── Boot-time framebuffer scroll probe (diagnostic fixture) ────
 *
 * Enabled with KERNEL_EXTRA_CFLAGS=-DOS01_TEST_FB_SCROLL and called
 * from x86_64_boot_devices() once the timer is calibrated and before
 * the display is handed over.
 *
 * It measures two things that must NOT be mixed:
 *   - seed_ns: the cost of building the shadow (the first copy of the
 *     visible pixels into the packed RAM buffer);
 *   - total_ns / fb_ns: the cost of a STEADY scroll, i.e. one that
 *     finds the shadow already in place.
 *
 * The log line is emitted AFTER Pos.lock and the writer lease are
 * released — no logging inside a lock.
 */
#ifdef OS01_TEST_FB_SCROLL

#include <log/log.h>

#define PRINTK_FB_SCROLL_SAMPLES 8

static uint64_t printk_fb_median(uint64_t *v, int n)
{
    for (int i = 1; i < n; i++) {
        uint64_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
    /* An even count: the median is the mean of the two middle ones. */
    return (v[n / 2 - 1] + v[n / 2]) / 2;
}

void printk_fb_scroll_probe(void)
{
    uint64_t seed_ns = 0;
    uint64_t total_ns[PRINTK_FB_SCROLL_SAMPLES];
    uint64_t fb_ns[PRINTK_FB_SCROLL_SAMPLES];
    uint32_t width = 0, height = 0, stride = 0;
    bool clock_ok = false;
    bool shadow_ok = false;

    for (int i = 0; i < PRINTK_FB_SCROLL_SAMPLES; i++) {
        total_ns[i] = 0;
        fb_ns[i] = 0;
    }

    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    fb_lease_t lease;
    int lrc = fb_writer_begin(&lease, 0);
    if (lrc == 0) {
        printk_fb_view_t view;
        if (printk_fb_view_resolve(&lease.snapshot, &view)) {
            width  = view.width;
            height = view.height;
            stride = view.stride;

            /* Drop a shadow left over from an earlier phase so the
             * seed cost below is the real one. */
            printk_fb_shadow_release();

            /* A clock that does not advance is not a clock. */
            uint64_t c0 = arch_clocksource_read_ns();
            uint64_t c1 = arch_clocksource_read_ns();
            clock_ok = (c1 != c0);

            if (clock_ok) {
                /* Seed: build the shadow and measure it on its own, so
                 * the steady-state samples carry no seed cost. */
                uint64_t s0 = arch_clocksource_read_ns();
                uint8_t *shadow = printk_fb_shadow_get(&view);
                uint64_t s1 = arch_clocksource_read_ns();
                seed_ns = s1 - s0;
                shadow_ok = (shadow != NULL);

                if (shadow_ok) {
                    /* Warm up along the SAME scroll path. */
                    (void)printk_fb_scroll_view(&view, font->height);

                    for (int i = 0; i < PRINTK_FB_SCROLL_SAMPLES; i++) {
                        uint64_t t0 = arch_clocksource_read_ns();
                        uint64_t f = printk_fb_scroll_view(&view, font->height);
                        uint64_t t1 = arch_clocksource_read_ns();
                        total_ns[i] = t1 - t0;
                        fb_ns[i] = f;
                    }
                }
            }
        }
        fb_writer_end(&lease);
    }
    spin_unlock_irqrestore(&Pos.lock, flags);

    /* The result is reported only after the lock and the lease are
     * gone. */
    if (!clock_ok) {
        log_info("[FB SCROLL] invalid: no clock (w=%u h=%u stride=%u)\n",
                 width, height, stride);
        return;
    }
    if (!shadow_ok) {
        log_info("[FB SCROLL] invalid: no shadow (w=%u h=%u stride=%u)\n",
                 width, height, stride);
        return;
    }

    uint64_t total_sorted[PRINTK_FB_SCROLL_SAMPLES];
    uint64_t fb_sorted[PRINTK_FB_SCROLL_SAMPLES];
    for (int i = 0; i < PRINTK_FB_SCROLL_SAMPLES; i++) {
        total_sorted[i] = total_ns[i];
        fb_sorted[i] = fb_ns[i];
    }
    uint64_t med_total = printk_fb_median(total_sorted, PRINTK_FB_SCROLL_SAMPLES);
    uint64_t med_fb = printk_fb_median(fb_sorted, PRINTK_FB_SCROLL_SAMPLES);

    log_info("[FB SCROLL] cfg w=%u h=%u stride=%u seed_ns=%llu\n",
             width, height, stride, seed_ns);
    log_info("[FB SCROLL] total_ns=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
             total_ns[0], total_ns[1], total_ns[2], total_ns[3],
             total_ns[4], total_ns[5], total_ns[6], total_ns[7]);
    log_info("[FB SCROLL] fb_ns=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
             fb_ns[0], fb_ns[1], fb_ns[2], fb_ns[3],
             fb_ns[4], fb_ns[5], fb_ns[6], fb_ns[7]);
    log_info("[FB SCROLL] median total_ns=%llu fb_ns=%llu\n", med_total, med_fb);
}

#endif /* OS01_TEST_FB_SCROLL */
