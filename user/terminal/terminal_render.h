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
} term_render_t;

/* Validate PSF2 header, dimensions, glyph capacity, and total size */
bool term_font_validate(const void *data, size_t size, const psf2_t **out_font);

/* Initialize render state */
void term_render_init(term_render_t *r, gfx_handle_t *gfx, const psf2_t *font,
                      term_core_t *core, uint32_t fg, uint32_t bg);

/* Check cursor delta and mark dirty cells accordingly */
bool term_render_cursor_update(term_render_t *r);

/* Flush dirty cells via gfx_draw_glyph and draw underline cursor */
void term_render_flush(term_render_t *r);

/* Clear full screen with background color using gfx_get_info() dimensions */
void term_render_clear(term_render_t *r);

#endif
