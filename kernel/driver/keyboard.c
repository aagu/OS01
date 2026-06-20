#include <stddef.h>
#include <stdbool.h>
#include <driver/keyboard.h>
#include <device/pic.h>
#include <kernel/apic.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/tty.h>
#include <kernel/arch/x86_64/hw.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <fs/vfs.h>

hw_int_controller_t keyboard_controller =
{
    .enable = pic_enable,
    .disable = pic_disable,
    .install = pic_install,
    .uninstall = pic_uninstall,
    .ack = pic_ack,
};

// ═══════════════════════════════════════════════════════════
//  Raw scancode ring buffer — used only by /dev/keyboard
//  Producer: IRQ handler.  Consumer: keyboard_read_scancodes().
// ═══════════════════════════════════════════════════════════

#define RING_SIZE 256

static volatile uint8_t scancode_ring[RING_SIZE];
static volatile int      ring_head;
static volatile int      ring_tail;

static inline int ring_full(void)
{
    int next = (ring_head + 1) % RING_SIZE;
    return next == ring_tail;
}

static inline int ring_empty(void)
{
    return ring_head == ring_tail;
}

// ═══════════════════════════════════════════════════════════
//  Scancode Set 1 → ASCII translation tables
// ═══════════════════════════════════════════════════════════

// Modifier state — updated inline in the IRQ handler.
// Global statics are safe here: the keyboard IRQ fires on
// exactly one CPU (BSP, IRQ1 affinity), and IRQs are
// disabled while the handler runs (no nesting on the same IRQ).

static bool kbd_lshift;
static bool kbd_rshift;
static bool kbd_lctrl;
static bool kbd_rctrl;
static bool kbd_lalt;
static bool kbd_ralt;
static bool kbd_caps_lock;

static inline bool kbd_shift(void) { return kbd_lshift || kbd_rshift; }
static inline bool kbd_ctrl(void)  { return kbd_lctrl  || kbd_rctrl;  }

struct kbd_key {
    char base;
    char shift;
};

static const struct kbd_key scancode_tbl[128] = {
    [0x01] = { '\x1b', '\x1b' }, [0x02] = { '1', '!' },
    [0x03] = { '2', '@' },       [0x04] = { '3', '#' },
    [0x05] = { '4', '$' },       [0x06] = { '5', '%' },
    [0x07] = { '6', '^' },       [0x08] = { '7', '&' },
    [0x09] = { '8', '*' },       [0x0A] = { '9', '(' },
    [0x0B] = { '0', ')' },       [0x0C] = { '-', '_' },
    [0x0D] = { '=', '+' },       [0x0E] = { '\b', '\b' },
    [0x0F] = { '\t', '\t' },     [0x10] = { 'q', 'Q' },
    [0x11] = { 'w', 'W' },       [0x12] = { 'e', 'E' },
    [0x13] = { 'r', 'R' },       [0x14] = { 't', 'T' },
    [0x15] = { 'y', 'Y' },       [0x16] = { 'u', 'U' },
    [0x17] = { 'i', 'I' },       [0x18] = { 'o', 'O' },
    [0x19] = { 'p', 'P' },       [0x1A] = { '[', '{' },
    [0x1B] = { ']', '}' },       [0x1C] = { '\n', '\n' },
    [0x1E] = { 'a', 'A' },       [0x1F] = { 's', 'S' },
    [0x20] = { 'd', 'D' },       [0x21] = { 'f', 'F' },
    [0x22] = { 'g', 'G' },       [0x23] = { 'h', 'H' },
    [0x24] = { 'j', 'J' },       [0x25] = { 'k', 'K' },
    [0x26] = { 'l', 'L' },       [0x27] = { ';', ':' },
    [0x28] = { '\'', '"' },      [0x29] = { '`', '~' },
    [0x2B] = { '\\', '|' },      [0x2C] = { 'z', 'Z' },
    [0x2D] = { 'x', 'X' },       [0x2E] = { 'c', 'C' },
    [0x2F] = { 'v', 'V' },       [0x30] = { 'b', 'B' },
    [0x31] = { 'n', 'N' },       [0x32] = { 'm', 'M' },
    [0x33] = { ',', '<' },       [0x34] = { '.', '>' },
    [0x35] = { '/', '?' },       [0x37] = { '*', '*' },
    [0x39] = { ' ', ' ' },       [0x4A] = { '-', '-' },
    [0x4E] = { '+', '+' },       [0x53] = { '\x7f', '\x7f' },
};

static const struct kbd_key ext_scancode_tbl[128] = {
    [0x1C] = { '\n', '\n' },     [0x35] = { '/', '/' },
    [0x47] = { 0, 0 },           [0x48] = { 0, 0 },
    [0x49] = { 0, 0 },           [0x4B] = { 0, 0 },
    [0x4D] = { 0, 0 },           [0x4F] = { 0, 0 },
    [0x50] = { 0, 0 },           [0x51] = { 0, 0 },
    [0x52] = { 0, 0 },           [0x53] = { '\x7f', '\x7f' },
};

// ═══════════════════════════════════════════════════════════
//  Inline scancode → ASCII translation (runs in IRQ context)
// ═══════════════════════════════════════════════════════════

// The TTY that receives translated characters.
static tty_t *kbd_tty = NULL;

// Translate one scancode byte and push ASCII to the TTY.
// `ext` is true when the byte was preceded by an E0 prefix.
// Returns nothing — the character (if any) is pushed via
// tty_push_input() which handles buffering and wakeup.
static void translate_and_push(uint8_t sc, bool ext)
{
    bool release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    // ── Modifier key tracking ─────────────────
    switch (code) {
    case 0x2A: kbd_lshift = !release; return;
    case 0x36: kbd_rshift = !release; return;
    case 0x1D:
        if (!ext) { kbd_lctrl = !release; return; }
        break;
    case 0x3A:
        if (!release) kbd_caps_lock = !kbd_caps_lock;
        return;
    case 0x38:
        if (!ext) { kbd_lalt = !release; return; }
        break;
    }
    if (ext) {
        switch (code) {
        case 0x1D: kbd_rctrl = !release; return;
        case 0x38: kbd_ralt  = !release; return;
        }
    }

    // Ignore key-release events for non-modifier keys
    if (release) return;

    // ── Look up ASCII ─────────────────────────
    const struct kbd_key *k = ext ? &ext_scancode_tbl[code]
                                  : &scancode_tbl[code];
    char c = 0;
    if (k->base != 0 || k->shift != 0) {
        bool caps = kbd_caps_lock;
        bool base_is_letter = (k->base >= 'a' && k->base <= 'z') ||
                              (k->base >= 'A' && k->base <= 'Z');
        if (caps && base_is_letter)
            c = kbd_shift() ? k->base : k->shift;
        else
            c = kbd_shift() ? k->shift : k->base;
    }
    if (c == 0) return;

    // ── Ctrl modifier ─────────────────────────
    if (kbd_ctrl()) {
        if (c >= 'a' && c <= 'z')      c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        else if (c == '[' || c == '{') c = '\x1b';
        else if (c == '\\' || c == '|') c = '\x1c';
        else if (c == ']' || c == '}') c = '\x1d';
        else if (c == '^' || c == '~') c = '\x1e';
        else if (c == '_' || c == '-') c = '\x1f';
        else if (c == '/')             c = '\x1f';
        else if (c == '2' || c == '@') c = '\x00';
        else if (c == '3')             c = '\x1b';
        else if (c == '4')             c = '\x1c';
        else if (c == '5')             c = '\x1d';
        else if (c == '6')             c = '\x1e';
        else if (c == '7')             c = '\x1f';
        else if (c == '8')             c = '\x7f';
    }

    if (c == 0) return;

    // Push to TTY — handles ring buffer + wakeup of blocked readers
    tty_push_input(kbd_tty, c);
}

// ═══════════════════════════════════════════════════════════
//  IRQ handler — port 0x60 → ring buffer + TTY
// ═══════════════════════════════════════════════════════════

void keyboard_handler(uint64_t nr, uint64_t parameter __attribute__((unused)),
                      pt_regs_t *regs __attribute__((unused)))
{
    uint8_t sc = inb(0x60);

    // Push raw scancode to ring buffer (for /dev/keyboard)
    if (!ring_full()) {
        scancode_ring[ring_head] = sc;
        __sync_synchronize();
        ring_head = (ring_head + 1) % RING_SIZE;
    }

    // Translate and push to TTY
    if (kbd_tty) {
        static bool e0_prefix = false;

        if (sc == 0xE0) {
            e0_prefix = true;
            return;
        }

        translate_and_push(sc, e0_prefix);
        e0_prefix = false;
    } else {
        // One-shot: keyboard IRQ fired before TTY was set up.
        // Write a lock-free debug marker to serial.
        static int warned = 0;
        if (!warned) {
            warned = 1;
            outb(0x3F8, '!');  // single byte — visible in serial output
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════

// Set the TTY that receives translated input.
void keyboard_set_tty(tty_t *tty)
{
    kbd_tty = tty;
}

// Poll the 8042 keyboard controller for available scancodes.
// Reads all pending bytes from port 0x60, translates them,
// and pushes ASCII characters to the given TTY.
// Must be called from task context (uses the same static
// e0_prefix as the IRQ handler — safe because IRQ handler
// runs atomically with respect to us).
void keyboard_poll(void)
{
    if (!kbd_tty) return;

    static bool poll_e0 = false;

    while (inb(0x64) & 1) {           // 8042 status: output buffer full
        uint8_t sc = inb(0x60);

        // Also push raw scancode to ring buffer
        if (!ring_full()) {
            scancode_ring[ring_head] = sc;
            __sync_synchronize();
            ring_head = (ring_head + 1) % RING_SIZE;
        }

        if (sc == 0xE0) {
            poll_e0 = true;
            continue;
        }

        translate_and_push(sc, poll_e0);
        poll_e0 = false;
    }
}

int keyboard_read_scancodes(uint8_t *buffer, int size)
{
    int count = 0;
    while (count < size && !ring_empty()) {
        buffer[count++] = scancode_ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return count;
}

// DevFS read handler for /dev/keyboard — raw scancodes
int keyboard_devfs_read(vfs_node_t *node, uint64_t offset,
                        uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    if (size > RING_SIZE) size = RING_SIZE;
    return keyboard_read_scancodes((uint8_t *)buffer, (int)size);
}

// ═══════════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════════

void keyboard_init(void)
{
    ring_head = 0;
    ring_tail = 0;
    kbd_lshift = false;
    kbd_rshift = false;
    kbd_lctrl  = false;
    kbd_rctrl  = false;
    kbd_lalt   = false;
    kbd_ralt   = false;
    kbd_caps_lock = false;

    hw_int_controller_t *ctrl = apic_available()
        ? get_ioapic_controller()
        : &keyboard_controller;
    register_irq(0x21, NULL, &keyboard_handler, 0, ctrl, "keyboard");
}
