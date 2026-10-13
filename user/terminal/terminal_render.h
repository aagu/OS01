#ifndef _TERMINAL_RENDER_H
#define _TERMINAL_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <gfx.h>
#include "terminal_core.h"

#define PSF2_MAGIC 0x864ab572u

/* PSF2 header: strictly 8 uint32 fields = 32 bytes */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
} __attribute__((packed)) psf2_t;

_Static_assert(sizeof(psf2_t) == 32, "psf2_t header must be exactly 32 bytes");

/* Task 4 (terminal-partial-present): pending-damage set kept by the
 * renderer between flush() calls and ack_damage().  Internal types — they
 * MUST NOT enter the kernel UAPI.  Constant cap = 64 rects.  When a
 * non-mergeable 65th span arrives the set collapses to full=true and the
 * rect list is discarded. */
#define TERM_DAMAGE_MAX_RECTS 64u

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} term_damage_rect_t;

typedef struct {
    bool full;
    uint32_t count;
    term_damage_rect_t rects[TERM_DAMAGE_MAX_RECTS];
} term_damage_t;

typedef struct {
    gfx_handle_t *gfx;
    const psf2_t *font;
    term_core_t  *core;
    uint32_t     fg;
    uint32_t     bg;
    int          last_cursor_col;
    int          last_cursor_row;
    bool         last_cursor_visible;
    bool         last_alt_active;
    /* Task 4: pending damage retained across flush() calls until
     * term_render_ack_damage() is called by the display layer after a
     * full logical-frame success.  full_pending starts true and any
     * term_render_clear()/pixel-scroll sets it true again. */
    term_damage_t pending_damage;
} term_render_t;

/* Validate PSF2 header, dimensions, glyph capacity, and total size */
bool term_font_validate(const void *data, size_t size, const psf2_t **out_font);

/* Initialize render state.  Defaults pending_damage.full = true so a
 * freshly-bound renderer does a full first present. */
void term_render_init(term_render_t *r, gfx_handle_t *gfx, const psf2_t *font,
                      term_core_t *core, uint32_t fg, uint32_t bg);

/* Check cursor delta and mark dirty cells accordingly */
bool term_render_cursor_update(term_render_t *r);

/* Flush dirty cells via gfx_draw_glyph and draw underline cursor.
 * Records row-spans into pending_damage BEFORE writing the first pixel
 * of each row; cursor drawing adds damage even when no cell is dirty. */
void term_render_flush(term_render_t *r);

/* Clear full screen with background color using gfx_get_info() dimensions.
 * Sets pending_damage.full = true and count = 0 (a fresh full view). */
void term_render_clear(term_render_t *r);

/* Snapshot pending_damage (full/count/rects[]) into *out.  Copy semantics
 * (caller owns the copy; the renderer keeps its own state). */
void term_render_get_damage(const term_render_t *r, term_damage_t *out);

/* Acknowledge that the caller has successfully presented the pending
 * damage.  Clears full + count to (false, 0).  Does NOT touch the
 * handle's pixels. */
void term_render_ack_damage(term_render_t *r);

#endif
