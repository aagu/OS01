#include <kernel/logo.h>
#include <kernel/printk.h>

// ── OS01 boot logo (7 rows x 31 cols ASCII art) ──────────────
// O S 0 1, drawn with putchar_at at character-cell granularity.
// Each letter is a 7x7 block; shapes VERIFIED by rendering (see
// docs/boot-logo-design.md / design script): O=RED, S=YELLOW,
// 0=GREEN, 1=INDIGO. Tagline + dividers use WHITE / LIGHT_GRAY.

#define LOGO_ROWS 7
#define LOGO_COLS 31

static const char *logo_lines[LOGO_ROWS] = {
    " #####   ######  #####     #   ",
    "#     # #     # #     #   ##   ",
    "#     # #       #   ###    #   ",
    "#     #  #####  #  #  #    #   ",
    "#     #       # ###   #    #   ",
    "#     # #     # #     #    #   ",
    " #####   ######  #####   ##### ",
};

// Letter column boundaries within a 31-char row (7 cols each + 1 gap):
//   O: 0-6   S: 8-14   0: 16-22   1: 24-30
static unsigned int logo_color_for_col(int col)
{
    if (col < 7)        return RED;
    if (col < 15)       return YELLOW;
    if (col < 23)       return GREEN;
    return INDIGO;                      // col 23..30 -> '1' region + padding
}

void boot_logo_show(void)
{
    int row, col;

    // Block 1: the OS01 letters.
    for (row = 0; row < LOGO_ROWS; row++) {
        for (col = 0; col < LOGO_COLS; col++) {
            char c = logo_lines[row][col];
            if (c == ' ')
                continue;               // leave background as-is
            putchar_at(col, row, logo_color_for_col(col), BLACK,
                       (unsigned char)c);
        }
    }

    // Block 2: divider.
    for (col = 0; col < LOGO_COLS; col++)
        putchar_at(col, LOGO_ROWS + 1, LIGHT_GRAY, BLACK, '=');

    // Block 3: tagline.
    const char *tag = "OS01 | x86-64 kernel";
    int t = 0;
    for (t = 0; tag[t] != '\0'; t++)
        putchar_at(t, LOGO_ROWS + 2, WHITE, BLACK, (unsigned char)tag[t]);

    // Block 4: bottom divider.
    for (col = 0; col < LOGO_COLS; col++)
        putchar_at(col, LOGO_ROWS + 3, LIGHT_GRAY, BLACK, '=');

    // Advance the software cursor below the logo so kernel log
    // output (color_printk) continues beneath the tagline.
    Pos.YPosition = LOGO_ROWS + 4;
    Pos.XPosition = 0;
}
