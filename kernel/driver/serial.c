#include <driver/serial.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/tty.h>
#include <kernel/apic.h>
#include <kernel/arch/io.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/irq.h>
#include <stddef.h>

// ═══════════════════════════════════════════════════════════
//  Ring buffer for received data
// ═══════════════════════════════════════════════════════════
// Producers: serial ISR (COM1 IRQ4, on BSP) and serial_poll()
//            (pit_handler IRQ0, on BSP).  Both run in IRQ
//            context on the same core — interrupt gates are
//            serialising (IF=0 on entry), so they're mutually
//            exclusive.  No per-Vector lock needed.
// Consumer:  read_serial() called from kernel tasks.
// SPSC variant: two mutually-exclusive writers, one reader.

#define SERIAL_RING_SIZE 256

static volatile char serial_rx_ring[SERIAL_RING_SIZE];
static volatile int  serial_rx_head;    // writer: serial ISR + serial_poll (BSP, mutually exclusive)
static volatile int  serial_rx_tail;    // reader: read_serial (task context)

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
//  Serial IRQ handler (COM1 IRQ4, GSI 4, vector 0x24)
// ═══════════════════════════════════════════════════════════
// Drains all available bytes from the UART RX FIFO and
// pushes them to the ring buffer.

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
    while (arch_inb(SERIAL_COM1 + 5) & 1) {
        char c = arch_inb(SERIAL_COM1);

        // I/O delay: allow the UART to update LSR bit 0 (Data Ready)
        // so the next while-condition read sees the correct state
        // instead of a stale value that could cause early loop exit
        // or double-reading the same byte.
        arch_inb(0x80);

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

    arch_outb(SERIAL_COM1 + 1, 0x00);    // Disable all interrupts (IER)
    arch_outb(SERIAL_COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    arch_outb(SERIAL_COM1 + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    arch_outb(SERIAL_COM1 + 1, 0x00);    //                  (hi byte)
    arch_outb(SERIAL_COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    arch_outb(SERIAL_COM1 + 2, 0xC7);    // Enable FIFO, clear, 14-byte trigger
    arch_outb(SERIAL_COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    // IER remains 0x00 here — RX interrupt enabled in init_serial_irq()
    // after the IOAPIC entry is unmasked, so there is no window where
    // the UART generates an IRQ that the IOAPIC has not yet routed.
}

void init_serial_irq(void)
{
    register_irq(4, NULL, &serial_handler, 0, IRQF_TRIGGER_LEVEL, "serial");

    // IOAPIC entry is now unmasked — safe to enable UART RX interrupts.
    arch_outb(SERIAL_COM1 + 1, 0x01);    // Enable RX interrupt (Data Available)

    // Diagnostic: read back UART interrupt config
    uint8_t ier = arch_inb(SERIAL_COM1 + 1);   // Interrupt Enable Register
    uint8_t iir = arch_inb(SERIAL_COM1 + 2);   // Interrupt Identification (also FCR)
    uint8_t mcr = arch_inb(SERIAL_COM1 + 4);   // Modem Control Register
    serial_printk("serial: IRQ4 registered (IER=%#x IIR=%#x MCR=%#x)\n",
                  ier, iir, mcr);
}

// ═══════════════════════════════════════════════════════════
//  Poll input (IRQ fallback)
// ═══════════════════════════════════════════════════════════

// Drain all available bytes from the UART and push to the
// console TTY.  Called from pit_handler (IRQ0, BSP, 100 Hz)
// as a fallback for the serial ISR.  Since this and
// serial_handler both fire on the BSP under interrupt gates
// (IF=0), they are mutually exclusive — no lock needed.
void serial_poll(void)
{
    if (!serial_tty) return;

    arch_irq_state_t irqf = arch_local_irq_save();

    while (arch_inb(SERIAL_COM1 + 5) & 1) {
        char c = arch_inb(SERIAL_COM1);

        // I/O delay: allow UART to update LSR before next check
        arch_inb(0x80);

        // Push to ring buffer (for /dev/serial)
        if (!serial_ring_full()) {
            serial_rx_ring[serial_rx_head] = c;
            __sync_synchronize();
            serial_rx_head = (serial_rx_head + 1) % SERIAL_RING_SIZE;
        }

        // Push to console TTY (wakes blocked tty_read)
        tty_push_input(serial_tty, c);
    }

    arch_local_irq_restore(irqf);
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
    while (!(arch_inb(SERIAL_COM1 + 5) & 1))
        arch_cpu_pause();

    char c = arch_inb(SERIAL_COM1);

    // I/O delay: give the UART time to update LSR bit 0 (Data Ready)
    // so the next serial_received() check doesn't see a stale 1.
    arch_inb(0x80);

    return c;
}

// Check if serial has received data (non-blocking).
bool serial_received(void)
{
    if (!serial_ring_empty())
        return true;
    return (arch_inb(SERIAL_COM1 + 5) & 1) != 0;
}

// ── Sending data ────────────────────────────────────────────

// Serial output lock — protects COM1 from interleaved writes
// when multiple code paths (tty_write, SYS_putchar, serial_printk,
// panic/stack_chk_fail) try to write concurrently.
// Used with plain spin_lock (not irqsave) because no interrupt
// handler calls write_serial or any function that acquires this
// lock while a task may already hold it.
spinlock_T serial_lock = {1};

static bool is_transmit_empty(void)
{
    return (arch_inb(SERIAL_COM1 + 5) & 0x20) != 0;
}

void write_serial(char c)
{
    while (!is_transmit_empty())
        arch_cpu_pause();

    arch_outb(SERIAL_COM1, c);
}
