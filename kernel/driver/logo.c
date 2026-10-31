#include <driver/logo.h>
#include <arch/fb_write.h>
#include <core/printk.h>
#include <driver/font.h>

extern psf2_t *font;

// A small, resolution-aware wordmark for the early 32-bit framebuffer.
// Each five-column glyph is drawn as solid geometry, without font assets.
#define LOGO_GLYPHS 4
#define GLYPH_ROWS 7
#define GLYPH_COLS 5

#define LOGO_INK    0x00e8f0f7
#define LOGO_ACCENT 0x003bd6c6
#define LOGO_MUTED  0x00687886

static const unsigned char glyphs[LOGO_GLYPHS][GLYPH_ROWS] = {
    { 0x0e, 0x1b, 0x11, 0x11, 0x11, 0x1b, 0x0e }, // O
    { 0x0f, 0x18, 0x10, 0x0e, 0x01, 0x03, 0x1e }, // S
    { 0x0e, 0x1b, 0x13, 0x15, 0x19, 0x1b, 0x0e }, // slashed 0
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, // 1
};

// Fill a rectangle in the framebuffer described by `view`.  The geometry is
// clipped to the framebuffer bounds; nothing is drawn outside it.
static void fill_rect(const struct fb_write_view *view, int x, int y,
                      int width, int height, unsigned int color)
{
    uint32_t xres = view->xres;
    uint32_t yres = view->yres;
    uint32_t *fb_base = view->addr;
    if (!fb_base) return;

    int right = x + width;
    int bottom = y + height;

    if (x >= (int)xres || y >= (int)yres ||
        right <= 0 || bottom <= 0)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int)xres) right = (int)xres;
    if (bottom > (int)yres) bottom = (int)yres;

    for (int py = y; py < bottom; py++) {
        uint32_t *pixel = fb_base + py * xres + x;
        for (int px = x; px < right; px++)
            *pixel++ = color;
    }
}

// Draw the wordmark and its caption into the framebuffer described by
// `view`.  The geometry is unchanged from the original x86_64-only
// implementation; only the framebuffer source differs per arch.
static void logo_draw(const struct fb_write_view *view, const char *label)
{
    uint32_t xres = view->xres;
    uint32_t yres = view->yres;

    int scale = xres >= 800 ? 14 :
                xres >= 480 ? 11 : 8;
    if (yres < 300)
        scale = 7;

    int left = xres >= 400 ? 32 : 16;
    int top = 24;
    int advance = 7 * scale;
    int width = (LOGO_GLYPHS - 1) * advance + GLYPH_COLS * scale;

    for (int letter = 0; letter < LOGO_GLYPHS; letter++) {
        unsigned int color = letter < 2 ? LOGO_INK : LOGO_ACCENT;
        for (int row = 0; row < GLYPH_ROWS; row++) {
            for (int col = 0; col < GLYPH_COLS; col++) {
                if (glyphs[letter][row] & (1u << (GLYPH_COLS - col - 1)))
                    fill_rect(view, left + letter * advance + col * scale,
                              top + row * scale, scale, scale, color);
            }
        }
    }

    int line_y = top + GLYPH_ROWS * scale + 14;
    fill_rect(view, left, line_y, 3 * scale, 2, LOGO_ACCENT);
    fill_rect(view, left + 3 * scale + 8, line_y,
              width - 3 * scale - 8, 1, LOGO_MUTED);

    int label_row = (line_y + 12 + (int)font->height - 1) / (int)font->height;
    int label_col = left / (int)font->width;
    for (int i = 0; label[i]; i++)
        putchar_at_snap(view->snap, label_col + i, label_row, LOGO_MUTED, BLACK,
                        (unsigned char)label[i]);

    // Leave a blank character row between the wordmark and boot messages.
    Pos.YPosition = label_row + 2;
    Pos.XPosition = 0;
}

int boot_logo_show(void)
{
    struct fb_write_view view;
    uint64_t flags = spin_lock_irqsave(&Pos.lock);

    int rc = arch_fb_write_begin(&view);
    if (rc < 0) {
        spin_unlock_irqrestore(&Pos.lock, flags);
        return rc;
    }

    logo_draw(&view, ARCH_BOOT_LOGO_LABEL);

    arch_fb_write_end(&view);
    spin_unlock_irqrestore(&Pos.lock, flags);
    return 0;
}
