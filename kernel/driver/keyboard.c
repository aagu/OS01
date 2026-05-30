#include <stddef.h>
#include <driver/keyboard.h>
#include <device/pic.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/hw.h>

hw_int_controller_t keyboard_controller =
{
    .enable = pic_enable,
    .disable = pic_disable,
    .install = pic_install,
    .uninstall = pic_uninstall,
    .ack = pic_ack,
};

// ──────────────────────────────────────────────
//  PS/2 scan code set 1 → ASCII mapping
// ──────────────────────────────────────────────

// Scancode press values ≤ 0x53
#define SCANCODE_MAX 0x53

// Shift flag tracking
static volatile uint8_t shift_pressed = 0;

// Default (unshifted) ASCII for each scancode, 0 = non-printable
static const char scancode_ascii[SCANCODE_MAX + 1] = {
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
    [0x2B] = '\\',    // Backslash (US layout)
    [0x2C] = 'z',     [0x2D] = 'x',     [0x2E] = 'c',
    [0x2F] = 'v',     [0x30] = 'b',     [0x31] = 'n',
    [0x32] = 'm',     [0x33] = ',',     [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' ',     // Space
    [0x47] = '7',     [0x48] = '8',     [0x49] = '9',   // Keypad
    [0x4B] = '4',     [0x4C] = '5',     [0x4D] = '6',
    [0x4F] = '1',     [0x50] = '2',     [0x51] = '3',
    [0x52] = '0',     [0x53] = '.',
};

// Shifted ASCII for each scancode
static const char scancode_shifted[SCANCODE_MAX + 1] = {
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

// Human-readable names for non-printable keys
static const char * scancode_name(uint8_t code)
{
    switch (code) {
    case 0x01: return "Esc";
    case 0x0E: return "Backspace";
    case 0x0F: return "Tab";
    case 0x1C: return "Enter";
    case 0x1D: return "Ctrl";
    case 0x2A: return "LShift";
    case 0x36: return "RShift";
    case 0x38: return "Alt";
    case 0x39: return "Space";
    case 0x3A: return "CapsLock";
    case 0x3B: case 0x3C: case 0x3D: case 0x3E:
    case 0x3F: case 0x40: case 0x41: case 0x42:
    case 0x43: case 0x44:
        return "F1-F10";
    case 0x45: return "NumLock";
    case 0x46: return "ScrollLock";
    default:  return NULL;
    }
}

void keyboard_handler(uint64_t nr, uint64_t parameter __attribute__((unused)),
                      pt_regs_t *regs __attribute__((unused)))
{
    uint8_t x = inb(0x60);
    outb(0x20, 0x20);   // EOI to PIC

    // Bit 7 set = key release
    uint8_t released = (x & 0x80) != 0;
    uint8_t code     = x & 0x7F;

    // Track shift state
    if (code == 0x2A || code == 0x36) {
        shift_pressed = released ? 0 : 1;
        return;
    }

    // Only process key press (ignore release of non-modifier keys)
    if (released)
        return;

    if (code > SCANCODE_MAX) {
        color_printk(GREEN, BLACK, "keyboard: unknown scancode %#04x\n", x);
        return;
    }

    char ch = shift_pressed ? scancode_shifted[code] : scancode_ascii[code];

    if (ch != 0) {
        // Printable character
        if (ch == '\b') {
            color_printk(GREEN, BLACK, "\b \b");
        } else if (ch == '\n') {
            color_printk(GREEN, BLACK, "\n");
        } else {
            color_printk(GREEN, BLACK, "%c", ch);
        }
        return;
    }

    // Non-printable key — show name
    const char *name = scancode_name(code);
    if (name) {
        color_printk(GREEN, BLACK, "[%s]", name);
    }
}

void keyboard_init()
{
    register_irq(0x21, NULL, &keyboard_handler, 0, &keyboard_controller, "keyboard");
}
