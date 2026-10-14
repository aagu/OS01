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

/* ── Pending-damage helpers (Task 4) ─────────────────────────────────
 *
 * The set is internal-only (no UAPI exposure).  Capacity is fixed at
 * TERM_DAMAGE_MAX_RECTS = 64 rects; a 65th non-mergeable span collapses
 * the set to (full=true, count=0).  Sort by (y asc, x asc) at the end
 * of each flush so display.c can iterate in a stable order.
 *
 * Merge rules (spec §6, brief):
 *   - Same y/h merges horizontally (x/w intervals combine when they
 *     overlap or touch — conservative overlap is allowed).
 *   - Same x/w merges vertically (rows when y/h consecutive — touching
 *     or overlapping — combine into a single tall rect).
 *   - The "try to merge" pass repeats until no rectangle could be
 *     merged further (stable fixed-point).
 *
 * Iteration order: try to merge FIRST, then check capacity.  Sort is
 * applied at flush boundary (sort_damage). */

/* Attempt to merge the new (cur_x, cur_y, cur_w, cur_h) into *r2.
 *
 * Returns true if a merge happened — *r2 now holds the union and the
 * caller should refresh (cur_x, cur_y, cur_w, cur_h) from *r2 before
 * continuing.  Returns false if no merge was possible.
 *
 * Conservative overlap is allowed (lost coverage would be a bug; lost
 * compactness is fine — duplicates may accumulate in the list).
 * Horizontal merge: same y/h, intervals overlap or touch.
 * Vertical merge: same x/w, intervals overlap or touch.
 */
static bool try_merge_into(term_damage_rect_t *r2,
                           uint32_t *cur_x, uint32_t *cur_y,
                           uint32_t *cur_w, uint32_t *cur_h)
{
    /* Horizontal: same y/h, x intervals overlap or touch. */
    if (r2->y == *cur_y && r2->h == *cur_h) {
        uint32_t r1l = r2->x,           r1r = r2->x + r2->w;
        uint32_t r2l = *cur_x,          r2r = *cur_x + *cur_w;
        if (r1r >= r2l && r1l <= r2r) {
            uint32_t nl = (r1l < r2l) ? r1l : r2l;
            uint32_t nr = (r1r > r2r) ? r1r : r2r;
            r2->x = nl;
            r2->w = nr - nl;
            *cur_x = r2->x; *cur_y = r2->y;
            *cur_w = r2->w; *cur_h = r2->h;
            return true;
        }
    }
    /* Vertical: same x/w, y intervals overlap or touch. */
    if (r2->x == *cur_x && r2->w == *cur_w) {
        uint32_t r1t = r2->y,           r1b = r2->y + r2->h;
        uint32_t r2t = *cur_y,          r2b = *cur_y + *cur_h;
        if (r1b >= r2t && r1t <= r2b) {
            uint32_t nt = (r1t < r2t) ? r1t : r2t;
            uint32_t nb = (r1b > r2b) ? r1b : r2b;
            r2->y = nt;
            r2->h = nb - nt;
            *cur_x = r2->x; *cur_y = r2->y;
            *cur_w = r2->w; *cur_h = r2->h;
            return true;
        }
    }
    return false;
}

/* Add one pixel span (x, y, w, h) to pending_damage, with merge.
 *
 * Order (brief §1):
 *   1. If pending_damage.full is already true, the list is discarded
 *      anyway — bail.
 *   2. Loop trying to merge into any existing rect until stable (no
 *      more merges can happen).  Touch the same index only once per
 *      outer pass to avoid no-op self-merge loops.
 *   3. If at least one merge happened the span was absorbed — done.
 *   4. Else, if count < TERM_DAMAGE_MAX_RECTS, append.
 *   5. Else, promote to full and discard the list.
 */
static void add_damage_span(term_render_t *r,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u) return;
    if (r->pending_damage.full) return;  /* list already discarded */

    uint32_t cur_x = x, cur_y = y, cur_w = w, cur_h = h;
    uint32_t touched = UINT32_MAX;
    bool absorbed = false;

    for (;;) {
        bool did = false;
        for (uint32_t i = 0; i < r->pending_damage.count; ++i) {
            if (i == touched) continue;  /* skip the one we last touched */
            term_damage_rect_t *r2 = &r->pending_damage.rects[i];
            if (try_merge_into(r2, &cur_x, &cur_y, &cur_w, &cur_h)) {
                did = true;
                absorbed = true;
                cur_x = r2->x; cur_y = r2->y;
                cur_w = r2->w; cur_h = r2->h;
                touched = i;
                break;
            }
        }
        if (!did) break;
    }

    if (absorbed) return;

    if (r->pending_damage.count >= TERM_DAMAGE_MAX_RECTS) {
        /* Overflow: discard list, promote to full. */
        r->pending_damage.full = true;
        r->pending_damage.count = 0;
        return;
    }
    term_damage_rect_t *r2 =
        &r->pending_damage.rects[r->pending_damage.count];
    r2->x = x; r2->y = y; r2->w = w; r2->h = h;
    r->pending_damage.count++;
}

/* Compare two damage rects by (y asc, x asc).  Used by sort_damage. */
static int cmp_rect_yx(const void *a, const void *b)
{
    const term_damage_rect_t *ra = (const term_damage_rect_t *)a;
    const term_damage_rect_t *rb = (const term_damage_rect_t *)b;
    if (ra->y != rb->y) return (ra->y < rb->y) ? -1 : 1;
    if (ra->x != rb->x) return (ra->x < rb->x) ? -1 : 1;
    return 0;
}

/* Sort pending_damage.rects by (y, x) ascending using a freestanding-
 * friendly insertion sort (capacity is at most 64, so O(n^2) is fine).
 * Stable on equal (y, x) keys (insertion sort doesn't reorder equals).
 */
static void sort_damage(term_render_t *r)
{
    uint32_t n = r->pending_damage.count;
    if (n < 2u) return;
    for (uint32_t i = 1; i < n; ++i) {
        term_damage_rect_t cur = r->pending_damage.rects[i];
        int32_t j = (int32_t)i;
        while (j > 0) {
            term_damage_rect_t *prev = &r->pending_damage.rects[j - 1];
            /* ascending sort by y first, then x */
            if (prev->y > cur.y ||
                (prev->y == cur.y && prev->x > cur.x)) {
                r->pending_damage.rects[j] = *prev;
                --j;
            } else {
                break;
            }
        }
        r->pending_damage.rects[j] = cur;
    }
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
    /* Task 4: a freshly-bound renderer starts with full=true so a first
     * present presents the entire view. */
    memset(&r->pending_damage, 0, sizeof(r->pending_damage));
    r->pending_damage.full = true;
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

    // Detect alt-screen transition: clear full view to erase old screen pixels.
    if (core->alt_active != r->last_alt_active) {
        term_render_clear(r);   /* also sets pending_damage.full = true */
        r->last_alt_active = core->alt_active;
        core->scroll_lines_pending = 0;
    }

    term_cell_t *screen = term_core_screen(core);
    if (!screen) return;

    // Handle pending pixel scrolling.
    if (core->scroll_lines_pending > 0) {
        int k = core->scroll_lines_pending;
        core->scroll_lines_pending = 0;
        if (k >= core->rows) {
            term_render_clear(r);
            term_core_mark_all_dirty(core);
            r->last_cursor_row = -1;
            r->last_cursor_visible = false;
        } else {
            /* Pixel scroll moved every destination pixel — entire view
             * changed, so promote to full and discard the rect list.
             * Spec §6 / brief: 像素 scroll (including sub-screen) is
             * always full pending. */
            int32_t shift_px = -(int32_t)(k * (int)font->height);
            gfx_scroll(r->gfx, 0, shift_px, r->bg);
            r->pending_damage.full = true;
            r->pending_damage.count = 0;

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
        /* Step 1: scan min/max dirty columns on this row and record the
         * pixel span in pending_damage BEFORE writing the first pixel.
         * Spec §6: damage must be recorded BEFORE pixels are written. */
        int min_col = -1, max_col = -1;
        for (int col = 0; col < core->cols; col++) {
            if (term_core_is_dirty(core, row, col)) {
                if (min_col < 0) min_col = col;
                max_col = col;
            }
        }
        if (min_col >= 0) {
            uint32_t x = (uint32_t)min_col * font->width;
            uint32_t y = (uint32_t)row * font->height;
            uint32_t w = (uint32_t)(max_col - min_col + 1) * font->width;
            uint32_t h = font->height;
            add_damage_span(r, x, y, w, h);
        }

        /* Step 2: actually draw the dirty cells (existing logic). */
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

    /* Render underline cursor if visible.  Spec §6: cursor drawing or
     * erase MUST be added to the damage set even when no cell is
     * dirty.  The underline row is the LAST pixel row of the cursor's
     * owning font cell, so it lives inside that cell's rect — when the
     * cell was dirty in this flush, the cursor's added span merges
     * into the cell rect (no-op).  When the cell is NOT dirty (e.g.
     * standalone visibility toggle), the cursor's 1-pixel-tall span
     * still gets recorded. */
    if (core->cursor_visible &&
        core->row >= 0 && core->row < core->rows &&
        core->col >= 0 && core->col < core->cols) {
        uint32_t cx = (uint32_t)core->col * font->width;
        uint32_t cy = (uint32_t)(core->row + 1) * font->height - 1u;
        add_damage_span(r, cx, cy, font->width, 1u);
        gfx_hline(r->gfx, (int32_t)cx, (int32_t)cy, font->width, r->fg);
    }

    /* Final stable order before returning to display. */
    sort_damage(r);

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
    /* Task 4: clear invalidates every previous rect — entire view is
     * dirty.  Set full=true and drop the list.  Subsequent add_damage_span
     * calls early-out because full is already set. */
    r->pending_damage.full = true;
    r->pending_damage.count = 0;
}

void term_render_get_damage(const term_render_t *r, term_damage_t *out)
{
    if (!r || !out) return;
    *out = r->pending_damage;
}

void term_render_ack_damage(term_render_t *r)
{
    if (!r) return;
    /* Spec §6: ack clears full+count without touching the handle's
     * pixels.  The renderer keeps its pixels as they are. */
    r->pending_damage.full = false;
    r->pending_damage.count = 0u;
}
