#include "terminal_render.h"
#include <string.h>

bool term_font_validate(const void *data, size_t size, const psf2_t **out_font)
{
    if (!data || size < sizeof(psf2_t)) return false;

    const psf2_t *hdr = (const psf2_t *)data;
    if (hdr->magic != PSF2_MAGIC) return false;
    if (hdr->headersize < sizeof(psf2_t) || hdr->headersize > size) return false;
    if (hdr->numglyph == 0u || hdr->bytesperglyph == 0u) return false;
    if (hdr->width == 0u || hdr->height == 0u) return false;

    // Check glyph capacity: bytesperglyph must hold at least ceil(width / 8) * height
    uint32_t row_bytes = hdr->width / 8u + ((hdr->width & 7u) != 0u);
    if (hdr->height > UINT32_MAX / row_bytes) return false;
    uint32_t min_bytes_per_glyph = row_bytes * hdr->height;
    if (hdr->bytesperglyph < min_bytes_per_glyph) return false;

    // Check total file size: headersize + numglyph * bytesperglyph <= size
    if (hdr->bytesperglyph > (SIZE_MAX - hdr->headersize) / hdr->numglyph) return false;
    size_t required_size = (size_t)hdr->headersize + (size_t)hdr->numglyph * (size_t)hdr->bytesperglyph;
    if (size < required_size) return false;

    if (out_font) *out_font = hdr;
    return true;
}

void term_render_init(term_render_t *r, gfx_handle_t *gfx, const psf2_t *font,
                      term_core_t *core, uint32_t fg, uint32_t bg)
{
    if (!r) return;
    r->gfx = gfx;
    r->font = font;
    r->core = core;
    r->fg = fg;
    r->bg = bg;
    r->last_cursor_col = -1;
    r->last_cursor_row = -1;
    r->last_cursor_visible = false;
    r->last_alt_active = false;
}

bool term_render_cursor_update(term_render_t *r)
{
    if (!r || !r->core) return false;
    term_core_t *core = r->core;
    if (!core->main_buf || !core->dirty || core->rows <= 0 || core->cols <= 0)
        return false;

    bool cursor_changed = (core->col != r->last_cursor_col ||
                           core->row != r->last_cursor_row ||
                           core->cursor_visible != r->last_cursor_visible);

    if (cursor_changed) {
        // Mark old cursor cell dirty to erase the old underline
        if (r->last_cursor_row >= 0 && r->last_cursor_row < core->rows &&
            r->last_cursor_col >= 0 && r->last_cursor_col < core->cols &&
            core->dirty) {
            core->dirty[r->last_cursor_row * core->cols + r->last_cursor_col] = true;
        }
        // Mark new cursor cell dirty
        if (core->row >= 0 && core->row < core->rows &&
            core->col >= 0 && core->col < core->cols &&
            core->dirty) {
            core->dirty[core->row * core->cols + core->col] = true;
        }
        return true;
    }
    return false;
}

void term_render_flush(term_render_t *r)
{
    if (!r || !r->core || !r->gfx || !r->font) return;
    term_core_t *core = r->core;
    const psf2_t *font = r->font;

    // Detect alt-screen transition: clear full view to erase old screen pixels
    if (core->alt_active != r->last_alt_active) {
        term_render_clear(r);
        r->last_alt_active = core->alt_active;
        core->scroll_lines_pending = 0;
    }

    term_cell_t *screen = term_core_screen(core);
    if (!screen) return;

    // Handle pending pixel scrolling
    if (core->scroll_lines_pending > 0) {
        int k = core->scroll_lines_pending;
        core->scroll_lines_pending = 0;
        if (k >= core->rows) {
            term_render_clear(r);
            term_core_mark_all_dirty(core);
            r->last_cursor_row = -1;
            r->last_cursor_visible = false;
        } else {
            int32_t shift_px = -(int32_t)(k * (int)font->height);
            gfx_scroll(r->gfx, 0, shift_px, r->bg);

            if (r->last_cursor_visible && r->last_cursor_row >= 0) {
                int shifted_cursor_row = r->last_cursor_row - k;
                if (shifted_cursor_row >= 0 && shifted_cursor_row < core->rows &&
                    r->last_cursor_col >= 0 && r->last_cursor_col < core->cols &&
                    core->dirty) {
                    core->dirty[shifted_cursor_row * core->cols + r->last_cursor_col] = true;
                }
                r->last_cursor_row = -1;
                r->last_cursor_visible = false;
            }
        }
    }

    uint32_t mask_stride = (font->width + 7u) / 8u;

    for (int row = 0; row < core->rows; row++) {
        for (int col = 0; col < core->cols; col++) {
            if (term_core_is_dirty(core, row, col)) {
                uint8_t g = screen[row * core->cols + col].glyph;
                uint32_t glyph_idx;
                if (g > 0 && (uint32_t)g < font->numglyph)
                    glyph_idx = (uint32_t)g;
                else if (' ' < font->numglyph)
                    glyph_idx = (uint32_t)' ';
                else
                    glyph_idx = 0;

                const uint8_t *mask = (const uint8_t *)font + font->headersize
                    + (size_t)glyph_idx * (size_t)font->bytesperglyph;

                gfx_draw_glyph(r->gfx,
                               col * (int32_t)font->width,
                               row * (int32_t)font->height,
                               mask, mask_stride,
                               font->width, font->height,
                               r->fg, r->bg, true);

                term_core_clear_dirty(core, row, col);
            }
        }
    }

    // Render underline cursor if visible
    if (core->cursor_visible &&
        core->row >= 0 && core->row < core->rows &&
        core->col >= 0 && core->col < core->cols) {
        gfx_hline(r->gfx,
                  core->col * (int32_t)font->width,
                  (core->row + 1) * (int32_t)font->height - 1,
                  font->width, r->fg);
    }

    r->last_cursor_col = core->col;
    r->last_cursor_row = core->row;
    r->last_cursor_visible = core->cursor_visible;
}

void term_render_clear(term_render_t *r)
{
    if (!r || !r->gfx) return;
    gfx_info_t info = gfx_get_info(r->gfx);
    if (info.width > 0 && info.height > 0) {
        gfx_fill_rect(r->gfx, 0, 0, info.width, info.height, r->bg);
    }
}
