#include <stddef.h>
#include <stdbool.h>
#include <driver/keyboard.h>
#include <kernel/apic.h>
#include <kernel/interrupt.h>
#include <kernel/debug.h>
#include <kernel/tty.h>
#include <kernel/arch/io.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/spinlock.h>
#include <fs/vfs.h>
#include <kernel/poll.h>
#include <kernel/percpu.h>
#include <kernel.h>

// ═══════════════════════════════════════════════════════════
//  Raw scancode ring buffer — used only by /dev/keyboard
//  Producer: IRQ handler.  Consumer: keyboard_read_scancodes().
// ═══════════════════════════════════════════════════════════

#define RING_SIZE 256

static volatile uint8_t scancode_ring[RING_SIZE];
static volatile int      ring_head;
static volatile int      ring_tail;

// ── Poll wait list — poll(2) waiters cascaded from IRQ context ──
static list_t kbd_poll;
static spinlock_T kbd_poll_lock;

static void keyboard_wake_pollers(void);

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

// Special key codes (> 0xFF, to avoid collision with ASCII).
// These are expanded to multi-byte VT100 escape sequences in
// translate_and_push when the TTY is in raw mode.
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_HOME  0x104
#define K_END   0x105

struct kbd_key {
    int base;
    int shift;
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
    [0x1C] = { '\n', '\n' },         // KP Enter
    [0x35] = { '/',  '/'  },         // KP /
    [0x47] = { K_HOME,  K_HOME  },   // E0 47 = Home
    [0x48] = { K_UP,    K_UP    },   // E0 48 = UP
    [0x49] = { K_UP,    K_UP    },   // E0 49 = Page Up (as UP for v1)
    [0x4B] = { K_LEFT,  K_LEFT  },   // E0 4B = LEFT
    [0x4D] = { K_RIGHT, K_RIGHT },   // E0 4D = RIGHT
    [0x4F] = { K_END,   K_END   },   // E0 4F = End
    [0x50] = { K_DOWN,  K_DOWN  },   // E0 50 = DOWN
    [0x51] = { K_DOWN,  K_DOWN  },   // E0 51 = Page Down (as DOWN for v1)
    [0x52] = { K_HOME,  K_HOME  },   // E0 52 = Insert (as Home for v1)
    [0x53] = { '\x7f', '\x7f' },     // E0 53 = Delete
};

// ═══════════════════════════════════════════════════════════
//  Inline scancode → ASCII translation (runs in IRQ context)
// ═══════════════════════════════════════════════════════════

// The TTY that receives translated characters.
static tty_t *kbd_tty = NULL;

// Push a VT100 CSI escape sequence to the TTY.
// seq is the final character: 'A'=UP, 'B'=DOWN, 'C'=RIGHT, 'D'=LEFT
static void push_vt100_seq(tty_t *tty, char seq)
{
    tty_push_input(tty, '\x1b');   // ESC
    tty_push_input(tty, '[');      // [
    tty_push_input(tty, seq);      // final
}

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
    int c = 0;
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

    // ── Expand VT100 escape sequences for navigation keys ──
    if (c >= 0x100) {
        switch (c) {
        case K_UP:    push_vt100_seq(kbd_tty, 'A'); break;
        case K_DOWN:  push_vt100_seq(kbd_tty, 'B'); break;
        case K_LEFT:  push_vt100_seq(kbd_tty, 'D'); break;
        case K_RIGHT: push_vt100_seq(kbd_tty, 'C'); break;
        case K_HOME:  push_vt100_seq(kbd_tty, 'H'); break;
        case K_END:   push_vt100_seq(kbd_tty, 'F'); break;
        default: break;
        }
        return;
    }

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
    uint8_t sc = arch_inb(0x60);

    // Push raw scancode to ring buffer (for /dev/keyboard)
    if (!ring_full()) {
        scancode_ring[ring_head] = sc;
        __sync_synchronize();
        ring_head = (ring_head + 1) % RING_SIZE;
    }
    keyboard_wake_pollers();

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
            arch_outb(0x3F8, '!');  // single byte — visible in serial output
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

tty_t *keyboard_get_tty(void)
{
    return kbd_tty;
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

    while (arch_inb(0x64) & 1) {           // 8042 status: output buffer full
        uint8_t sc = arch_inb(0x60);

        // Also push raw scancode to ring buffer
        if (!ring_full()) {
            scancode_ring[ring_head] = sc;
            __sync_synchronize();
            ring_head = (ring_head + 1) % RING_SIZE;
        }
        keyboard_wake_pollers();

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

// ═══════════════════════════════════════════════════════════
//  Poll support — /dev/keyboard readiness
// ═══════════════════════════════════════════════════════════

static void keyboard_wake_pollers(void)
{
    uint64_t flags = spin_lock_irqsave(&kbd_poll_lock);
    while (!list_is_empty(&kbd_poll)) {
        list_t *node = kbd_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
    spin_unlock_irqrestore(&kbd_poll_lock, flags);

    this_cpu()->need_resched = 1;
}

uint32_t keyboard_poll_dev(void *priv, uint32_t requested, poll_table_t *pt)
{
    (void)priv;
    uint32_t mask = 0;

    if (!ring_empty()) {
        mask |= POLLIN | POLLRDNORM;
    } else if (poll_requested_read(requested) && pt && !pt->triggered) {
        poll_wait(pt, &kbd_poll, &kbd_poll_lock);
    }
    return mask;
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

// Enable keyboard IRQ generation at the PS/2 controller level.
// The controller's command byte bit 0 (keyboard interrupt enable)
// may be 0 after UEFI boot — without it, IRQ1 never fires.
static void keyboard_enable_irq(void)
{
    // Write command 0x20 ("read command byte") to port 0x64
    while (arch_inb(0x64) & 2) arch_cpu_pause();  // wait for input buffer empty
    arch_outb(0x64, 0x20);
    // Read response from port 0x60
    while (!(arch_inb(0x64) & 1)) arch_cpu_pause();  // wait for output buffer full
    uint8_t cmd = arch_inb(0x60);

    debug_irq("kbd: PS/2 command byte was %#x", cmd);

    // Bit 0: keyboard interrupt enable
    // Bit 2: system flag (tell controller we passed POST)
    // Bit 3: keyboard enable (0 = enable, 1 = disable)
    cmd |= 0x01;   // enable IRQ1
    cmd |= 0x04;   // set system flag
    cmd &= ~0x08;  // ensure keyboard is enabled

    // Write command 0x60 ("write command byte") to port 0x64
    while (arch_inb(0x64) & 2) arch_cpu_pause();
    arch_outb(0x64, 0x60);
    // Write the byte to port 0x60
    while (arch_inb(0x64) & 2) arch_cpu_pause();
    arch_outb(0x60, cmd);

    debug_irq(" → %#x\n", cmd);
}

void keyboard_init(void)
{
    ring_head = 0;
    ring_tail = 0;
    list_init(&kbd_poll);
    spin_init(&kbd_poll_lock);
    kbd_lshift = false;
    kbd_rshift = false;
    kbd_lctrl  = false;
    kbd_rctrl  = false;
    kbd_lalt   = false;
    kbd_ralt   = false;
    kbd_caps_lock = false;

    keyboard_enable_irq();

    register_irq(1, NULL, &keyboard_handler, 0, IRQF_TRIGGER_EDGE, "keyboard");
}
