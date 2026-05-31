#include <stddef.h>
#include <driver/keyboard.h>
#include <device/pic.h>
#include <kernel/apic.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
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

// ── Scancode ring buffer ─────────────────────────────────
// Producer: IRQ handler (IRQ1).  Consumer: keyboard_read_scancodes().
// SPSC: safe without locks — just volatile + ordering.

#define RING_SIZE 256

static volatile uint8_t scancode_ring[RING_SIZE];
static volatile int      ring_head;   // producer writes here (IRQ)
static volatile int      ring_tail;   // consumer reads here (read func)

static inline int ring_full(void)
{
    int next = (ring_head + 1) % RING_SIZE;
    return next == ring_tail;
}

static inline int ring_empty(void)
{
    return ring_head == ring_tail;
}

// ── IRQ handler ──────────────────────────────────────────
// Pushes the raw port-0x60 byte to the ring buffer.  No
// ASCII translation, no state tracking — pure collection.

void keyboard_handler(uint64_t nr, uint64_t parameter __attribute__((unused)),
                      pt_regs_t *regs __attribute__((unused)))
{
    uint8_t x = inb(0x60);

    if (ring_full())
        return;                         // drop on overflow

    scancode_ring[ring_head] = x;
    __sync_synchronize();               // ensure write visible before bump
    ring_head = (ring_head + 1) % RING_SIZE;
}

// ── Public API ───────────────────────────────────────────

int keyboard_read_scancodes(uint8_t *buffer, int size)
{
    int count = 0;
    while (count < size && !ring_empty()) {
        buffer[count++] = scancode_ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return count;
}

// ── DevFS read handler for /dev/keyboard ─────────────────

int keyboard_devfs_read(vfs_node_t *node, uint64_t offset,
                        uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    if (size > RING_SIZE) size = RING_SIZE;
    return keyboard_read_scancodes((uint8_t *)buffer, (int)size);
}

// ── Init ─────────────────────────────────────────────────

void keyboard_init()
{
    ring_head = 0;
    ring_tail = 0;

    hw_int_controller_t *ctrl = apic_available()
        ? get_ioapic_controller()
        : &keyboard_controller;
    register_irq(0x21, NULL, &keyboard_handler, 0, ctrl, "keyboard");
}
