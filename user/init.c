#include <stdio.h>
#include <unistd.h>
#include <stdint.h>

// ── PS/2 scan code set 1 → ASCII mapping (unshifted) ─────
// 0 = non-printable / handled specially
#define SCANCODE_MAX 0x53

static const char ascii_tbl[SCANCODE_MAX + 1] = {
    [0x01] = 0x1B,    // Escape
    [0x02] = '1',     [0x03] = '2',     [0x04] = '3',
    [0x05] = '4',     [0x06] = '5',     [0x07] = '6',
    [0x08] = '7',     [0x09] = '8',     [0x0A] = '9',
    [0x0B] = '0',     [0x0C] = '-',     [0x0D] = '=',
    [0x0E] = '\b',    // Backspace
    [0x0F] = '\t',    // Tab
    [0x10] = 'q',     [0x11] = 'w',     [0x12] = 'e',
    [0x13] = 'r',     [0x14] = 't',     [0x15] = 'y',
    [0x16] = 'u',     [0x17] = 'i',     [0x18] = 'o',
    [0x19] = 'p',     [0x1A] = '[',     [0x1B] = ']',
    [0x1C] = '\n',    // Enter
    [0x1E] = 'a',     [0x1F] = 's',     [0x20] = 'd',
    [0x21] = 'f',     [0x22] = 'g',     [0x23] = 'h',
    [0x24] = 'j',     [0x25] = 'k',     [0x26] = 'l',
    [0x27] = ';',     [0x28] = '\'',    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',     [0x2D] = 'x',     [0x2E] = 'c',
    [0x2F] = 'v',     [0x30] = 'b',     [0x31] = 'n',
    [0x32] = 'm',     [0x33] = ',',     [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' ',     // Space
};

static const char shifted_tbl[SCANCODE_MAX + 1] = {
    [0x02] = '!',     [0x03] = '@',     [0x04] = '#',
    [0x05] = '$',     [0x06] = '%',     [0x07] = '^',
    [0x08] = '&',     [0x09] = '*',     [0x0A] = '(',
    [0x0B] = ')',     [0x0C] = '_',     [0x0D] = '+',
    [0x10] = 'Q',     [0x11] = 'W',     [0x12] = 'E',
    [0x13] = 'R',     [0x14] = 'T',     [0x15] = 'Y',
    [0x16] = 'U',     [0x17] = 'I',     [0x18] = 'O',
    [0x19] = 'P',     [0x1A] = '{',     [0x1B] = '}',
    [0x1E] = 'A',     [0x1F] = 'S',     [0x20] = 'D',
    [0x21] = 'F',     [0x22] = 'G',     [0x23] = 'H',
    [0x24] = 'J',     [0x25] = 'K',     [0x26] = 'L',
    [0x27] = ':',     [0x28] = '"',     [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',     [0x2D] = 'X',     [0x2E] = 'C',
    [0x2F] = 'V',     [0x30] = 'B',     [0x31] = 'N',
    [0x32] = 'M',     [0x33] = '<',     [0x34] = '>',
    [0x35] = '?',     [0x39] = ' ',
};

// Modifier scancodes
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_ALT      0x38
#define SC_CAPSLOCK 0x3A

// ── Main ─────────────────────────────────────────────────

int main(void)
{
    int shift_pressed = 0;
    int capslock = 0;

    // fd 0 = /dev/keyboard (inherited from init task)
    // fd 1 = /dev/fb        (inherited)
    // fd 2 = /dev/serial     (inherited)
    printf("Init: keyboard echo ready\n");

    while (1) {
        uint8_t sc;
        int64_t n = read(0, &sc, 1);   // fd 0 = stdin (/dev/keyboard)
        if (n <= 0)
            continue;

        uint8_t code  = sc & 0x7F;
        int released   = (sc >> 7) & 1;

        // ── Track modifier state ─────────────────────────
        if (code == SC_CTRL || code == SC_ALT) {
            // Not used for now, but tracking is trivial
            continue;
        }
        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            shift_pressed = released ? 0 : 1;
            continue;
        }
        if (code == SC_CAPSLOCK) {
            if (!released)
                capslock = !capslock;
            continue;
        }

        // Ignore releases of non-modifier keys
        if (released)
            continue;

        // ── Look up the character ────────────────────────
        if (code > SCANCODE_MAX)
            continue;

        char ch;
        int use_shift = shift_pressed ^ capslock;
        ch = use_shift ? shifted_tbl[code] : ascii_tbl[code];

        if (ch == 0)
            continue;

        // ── Handle special characters ────────────────────
        if (ch == '\b') {
            printf("\b \b");        // erase previous character
        } else {
            printf("%c", ch);
        }
    }

    return 0;
}
