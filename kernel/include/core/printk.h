#ifndef _KERNEL_PRINTK_H
#define _KERNEL_PRINTK_H

#include <stdbool.h>
#include <stdint.h>
#include <arch/spinlock.h>

#define WHITE 	0x00ffffff		//白
#define BLACK 	0x00000000		//黑
#define RED	    0x00ff0000		//红
#define ORANGE	0x00ff8000		//橙
#define YELLOW	0x00ffff00		//黄
#define GREEN	0x0000ff00		//绿
#define BLUE	0x000000ff		//蓝
#define INDIGO	0x0000ffff		//靛
#define PURPLE	0x008000ff		//紫
#define LIGHT_GRAY	0x00c0c0c0	//浅灰

// Temporary framebuffer address used before pmm/vmm are available.
// Within PUD[0] for simple direct-PDE-write setup in frame_buffer_early_init().
// Placed at 1008MB (PDE index 504) — above all physical RAM for QEMU -m up to ~1GB.
#define VIRT_FRAMEBUFFER_EARLY 0xFFFF80003F000000

// Permanent framebuffer address, remapped by frame_buffer_init() after VMM.
// Uses a separate PGD entry (PGD[288], 16TB above higher-half base) that
// will NEVER overlap with the physical RAM direct mapping (PGD[256..287]),
// regardless of QEMU -m size.
#define VIRT_FRAMEBUFFER_OFFSET 0xFFFF900000000000

typedef struct position
{
	int32_t XResolution;
	int32_t YResolution;

	int32_t XPosition;
	int32_t YPosition;

	uint32_t * Phy_addr;
	uint32_t * FB_addr; // memory should be write in 32bits block
	uint64_t FB_length;

	spinlock_T lock;
} position;

extern position Pos;

struct fb_snapshot;

void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char font);
void putchark_snap(const struct fb_snapshot *snap, unsigned int FRcolor, unsigned int BKcolor, unsigned char font);
// Write a character to a specific character-cell position on the
// framebuffer.  (col, row) are in character cells; pixel address
// is computed from font->width/height and Pos.FB_addr.
// Does NOT touch Pos.XPosition/YPosition or Pos.lock.
void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c);
void putchar_at_snap(const struct fb_snapshot *snap, int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                     unsigned char c);
int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...);
int printk_fb_write(unsigned int FRcolor, unsigned int BKcolor, const char *buf, int len);
int serial_printk(const char *fmt,...);

// ── Framebuffer view (geometry + writable range) ────────────────
//
// A resolved framebuffer view: the geometry a writer may touch.  The
// console resolves this ONCE per write from the writer lease
// snapshot, so the glyph path, the logo path and the scroll path all
// agree on the same width, height, real byte pitch and writable
// range.  A framebuffer whose pitch is larger than width*4 (a
// 1024x768 panel with a 4096-byte pitch) is the common real-hardware
// case, and every offset MUST be computed from `stride`.
//
//   fb        framebuffer base, NULL when there is none
//   width     VISIBLE pixels per row
//   height    VISIBLE pixel rows
//   stride    framebuffer byte pitch (>= width*4)
//   fb_bytes  writable bytes at `fb`; 0 means "unbounded"
typedef struct {
    uint8_t  *fb;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
    uint64_t  fb_bytes;
} printk_fb_view_t;

// The geometry is usable when there is a framebuffer, a non-empty
// visible area and a pitch that can hold one visible row.
static inline bool printk_fb_view_ok(const printk_fb_view_t *v)
{
    return v && v->fb && v->width && v->height && v->stride &&
           (uint64_t)v->width * 4u <= (uint64_t)v->stride;
}

// Byte offset of the FIRST VISIBLE pixel of pixel row `y`.
//
// Fails when the row does not fit inside the writable range.  The
// check covers the WHOLE pitch extent of the row, not just its
// visible pixels: a framebuffer whose last row is truncated is a
// broken geometry, and a writer must refuse it instead of painting a
// partial frame.
//
// A `fb_bytes` of 0 means "unbounded" (the legacy no-snapshot path,
// where the framebuffer size is not published).
static inline bool printk_fb_row_offset(const printk_fb_view_t *v, uint32_t y,
                                         uint64_t *off)
{
    if (!printk_fb_view_ok(v) || y >= v->height)
        return false;

    uint64_t o = (uint64_t)y * (uint64_t)v->stride;
    if (v->fb_bytes != 0 && o + v->stride > v->fb_bytes)
        return false;

    *off = o;
    return true;
}

// Resolve the view from a writer lease snapshot.  A NULL snapshot
// falls back to the legacy Pos mirrors (the aarch64 path).  Returns
// false when there is no framebuffer or the geometry is empty.
bool printk_fb_view_resolve(const struct fb_snapshot *snap, printk_fb_view_t *out);

// Scroll the visible framebuffer up by `shift_rows` VISIBLE pixel
// rows.  The caller MUST hold Pos.lock and a writer lease (or
// otherwise guarantee that no other writer is active).
void printk_fb_scroll_view(const printk_fb_view_t *view, uint32_t shift_rows);

// The boot-log shadow (x86 + FB_SHADOW builds only).  The shadow holds
// the VISIBLE pixels of the framebuffer in a PACKED row layout (row
// stride = width*4), so the framebuffer itself is always addressed by
// its real pitch.  Returns NULL when the shadow is unavailable (PMM
// not ready, allocation failure, mode switch) — the caller then falls
// back to the direct scroll.
uint8_t *printk_fb_shadow_get(const printk_fb_view_t *view);
void printk_fb_shadow_release(void);

void frame_buffer_init();
void frame_buffer_early_init();

#endif
