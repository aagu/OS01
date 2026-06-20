#include <driver/serial.h>
#include <kernel/arch/x86_64/hw.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/tty.h>
#include <device/pic.h>
#include <kernel/apic.h>
#include <stddef.h>

// ═══════════════════════════════════════════════════════════
//  Ring buffer for received data
// ═══════════════════════════════════════════════════════════
// Producer: serial IRQ handler (COM1 IRQ4).
// Consumer: read_serial() called from kernel tasks.
// SPSC: head written only by IRQ, tail only by task context.

#define SERIAL_RING_SIZE 256

static volatile char serial_rx_ring[SERIAL_RING_SIZE];
static volatile int  serial_rx_head;    // IRQ writes here
static volatile int  serial_rx_tail;    // read_serial reads here

static inline bool serial_ring_full(void)
{
    int next = (serial_rx_head + 1) % SERIAL_RING_SIZE;
    return next == serial_rx_tail;
}

static inline bool serial_ring_empty(void)
{
    return serial_rx_head == serial_rx_tail;
}

// ═══════════════════════════════════════════════════════════
//  Serial IRQ handler (COM1 IRQ4, vector 0x24)
// ═══════════════════════════════════════════════════════════
// Drains all available bytes from the UART RX FIFO and
// pushes them to the ring buffer.

static hw_int_controller_t serial_controller =
{
    .enable   = pic_enable,
    .disable  = pic_disable,
    .install  = pic_install,
    .uninstall = pic_uninstall,
    .ack      = pic_ack,
};

// Console TTY — serial handler pushes received bytes here.
static tty_t *serial_tty = NULL;

void serial_set_tty(tty_t *tty)
{
    serial_tty = tty;
}

static void serial_handler(uint64_t nr, uint64_t parameter __attribute__((unused)),
                           pt_regs_t *regs __attribute__((unused)))
{
    (void)nr;
    // Drain the RX FIFO
    while (inb(SERIAL_COM1 + 5) & 1) {
        char c = inb(SERIAL_COM1);

        // Push to serial ring buffer (for /dev/serial)
        if (!serial_ring_full()) {
            serial_rx_ring[serial_rx_head] = c;
            __sync_synchronize();
            serial_rx_head = (serial_rx_head + 1) % SERIAL_RING_SIZE;
        }

        // Also push to the console TTY so terminal input works
        tty_push_input(serial_tty, c);
    }
}

// ═══════════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════════

void init_serial(void)
{
    serial_rx_head = 0;
    serial_rx_tail = 0;

    outb(SERIAL_COM1 + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_COM1 + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_COM1 + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_COM1 + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

void init_serial_irq(void)
{
    hw_int_controller_t *ctrl = apic_available()
        ? get_ioapic_controller()
        : &serial_controller;
    register_irq(0x24, NULL, &serial_handler, 0, ctrl, "serial");
    serial_printk("serial: IRQ4 registered\n");
}

// ═══════════════════════════════════════════════════════════
//  Read / write
// ═══════════════════════════════════════════════════════════

// Read one character from serial input.
// Drains the IRQ ring buffer first; falls back to polling
// (busy-wait) if the ring is empty (e.g. before IRQs are set up).
char read_serial(void)
{
    // Drain IRQ ring buffer
    if (!serial_ring_empty()) {
        char c = serial_rx_ring[serial_rx_tail];
        serial_rx_tail = (serial_rx_tail + 1) % SERIAL_RING_SIZE;
        return c;
    }

    // Fallback: poll the hardware directly
    while (!(inb(SERIAL_COM1 + 5) & 1))
        __asm__ __volatile__("pause");

    return inb(SERIAL_COM1);
}

// Check if serial has received data (non-blocking).
bool serial_received(void)
{
    if (!serial_ring_empty())
        return true;
    return (inb(SERIAL_COM1 + 5) & 1) != 0;
}

// ── Sending data ────────────────────────────────────────────

static bool is_transmit_empty(void)
{
    return (inb(SERIAL_COM1 + 5) & 0x20) != 0;
}

void write_serial(char c)
{
    while (!is_transmit_empty())
        __asm__ __volatile__("pause");

    outb(SERIAL_COM1, c);
}
