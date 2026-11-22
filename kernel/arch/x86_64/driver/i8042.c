// i8042.c — PS/2 controller (8042) shared port-ownership layer.
//
// This is the ONLY file in the kernel allowed to touch ports 0x64/0x60
// (spec: docs/superpowers/specs/2026-09-26-ps2-mouse-driver-design.md §4).
// Known, accepted exception: the panic path in
// kernel/arch/x86_64/intr/trap.c writes 0x64 <- 0xFE directly (pulse reset)
// to reboot the machine — the system has already panicked and is about to
// reset, so not taking the lock there is expected.
// keyboard.c and mouse.c receive already-demultiplexed bytes through the
// consumer callbacks installed here.
//
// Core invariants:
//   * status (0x64) and its data byte (0x60) are read as a PAIR inside a
//     single spin_lock_irqsave(i8042_lock) critical section — the IRQ line
//     (1 vs 12) is only a wake-up hint, ownership is decided by status
//     bit 5 (AUX) at read time;
//   * 0x60 is never read when OBF (bit 0) is clear;
//   * consumers are invoked in-lock and must not touch the i8042 ports
//     or re-take the lock;
//   * command transactions (0x20 / 0x60+value / 0xD4+data) run as single
//     locked, local-IRQs-off critical sections with bounded busy-waits.
//
// Timing deliberately uses clocksource_cycles()/clocksource_freq_hz()
// (declared as externs below to avoid pulling scheduler/percpu headers):
// clocksource_read_ns() touches this_cpu() via GS base, which is NOT
// installed yet at phase 5/6 init time.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>         // NULL

#include <driver/i8042.h>
#include <arch/io.h>
#include <arch/cpu.h>        // arch_cpu_pause
#include <arch/spinlock.h>

// Raw clocksource API — signatures match <time/clocksource.h>. Declared
// locally so this file stays compilable for host tests without the full
// scheduler header tree.
extern uint64_t clocksource_cycles(void);
extern uint64_t clocksource_freq_hz(void);

#define I8042_DATA_PORT   0x60
#define I8042_STATUS_PORT 0x64

#define I8042_STS_OBF 0x01   /* output buffer full  */
#define I8042_STS_IBF 0x02   /* input buffer full   */
#define I8042_STS_AUX 0x20   /* byte came from aux  */

#define I8042_CMD_READ_CB 0x20   /* read command byte      */
#define I8042_CMD_WRITE_CB 0x60  /* write command byte     */
#define I8042_CMD_AUX_WRITE 0xD4 /* write to aux (mouse)   */

// Per-wait upper bound for IBF/OBF busy-waits, and the overall budget of
// one transaction.
#define I8042_WAIT_MS 20

static spinlock_T i8042_lock;
static i8042_consumer_t kbd_consumer;
static i8042_consumer_t aux_consumer;
static bool inited;

// ── Deadline helpers (assume lock held + local IRQs off) ────────────

// true once the virtual clock passes `deadline`.
static inline bool i8042_expired(uint64_t deadline)
{
    return (int64_t)(clocksource_cycles() - deadline) >= 0;
}

static int i8042_deadline_new(uint64_t *out)
{
    uint64_t freq = clocksource_freq_hz();
    if (freq == 0)
        return I8042_ERR_NO_CLOCK;
    *out = clocksource_cycles() + freq * I8042_WAIT_MS / 1000;
    return I8042_OK;
}

// Wait until the controller accepts input (IBF empty). Bounded by the
// transaction deadline.
static int i8042_wait_ibf_empty(uint64_t deadline)
{
    while (arch_inb(I8042_STATUS_PORT) & I8042_STS_IBF) {
        arch_cpu_pause();
        if (i8042_expired(deadline))
            return I8042_ERR_TIMEOUT;
    }
    return I8042_OK;
}

// Wait until the controller produced a byte (OBF full). Bounded by the
// transaction deadline.
static int i8042_wait_obf_full(uint64_t deadline)
{
    while (!(arch_inb(I8042_STATUS_PORT) & I8042_STS_OBF)) {
        arch_cpu_pause();
        if (i8042_expired(deadline))
            return I8042_ERR_TIMEOUT;
    }
    return I8042_OK;
}

// One status+data pair read and dispatch. Lock must be held; the consumer
// runs in-lock. Returns the source of the byte (I8042_SRC_NONE when the
// output buffer was empty and nothing was read).
static i8042_src_t i8042_read_and_dispatch(void)
{
    uint8_t st = arch_inb(I8042_STATUS_PORT);
    if (!(st & I8042_STS_OBF))
        return I8042_SRC_NONE;          // never read 0x60 on empty OBF

    uint8_t data = arch_inb(I8042_DATA_PORT);
    if (st & I8042_STS_AUX) {
        if (aux_consumer)
            aux_consumer(data);         // else: drop (pre-mouse stale byte)
        return I8042_SRC_AUX;
    }
    if (kbd_consumer)
        kbd_consumer(data);
    return I8042_SRC_KBD;
}

// Drain every currently pending OBF byte through the normal demux rules,
// bounded by the transaction deadline (a flooding device must not starve
// the command issue).
static int i8042_drain_all(uint64_t deadline)
{
    for (;;) {
        if (i8042_read_and_dispatch() == I8042_SRC_NONE)
            return I8042_OK;            // output buffer confirmed empty
        if (i8042_expired(deadline))
            return I8042_ERR_TIMEOUT;
    }
}

// ── Public API ──────────────────────────────────────────────────────

void i8042_init(void)
{
    if (inited)
        return;
    spin_init(&i8042_lock);
    kbd_consumer = NULL;
    aux_consumer = NULL;
    inited = true;
}

void i8042_set_kbd_consumer(i8042_consumer_t consumer)
{
    kbd_consumer = consumer;
}

void i8042_set_aux_consumer(i8042_consumer_t consumer)
{
    aux_consumer = consumer;
}

// Upper bound on bytes drained by one i8042_pump() call. A healthy
// controller presents at most a handful of bytes per IRQ; a stuck OBF
// status line (faulty hardware) must not spin forever while holding the
// lock with local IRQs off.
#define I8042_PUMP_MAX_BYTES 32

int i8042_pump(void)
{
    int rc = I8042_OK;

    uint64_t flags = spin_lock_irqsave(&i8042_lock);
    for (int drained = 0; drained < I8042_PUMP_MAX_BYTES; drained++) {
        if (i8042_read_and_dispatch() == I8042_SRC_NONE)
            goto out;                   /* output buffer drained empty */
    }
    /* OBF still asserted after the cap: give up and report it instead of
     * spinning forever in-lock with IRQs off. */
    rc = I8042_ERR_TIMEOUT;

out:
    spin_unlock_irqrestore(&i8042_lock, flags);
    return rc;
}

int i8042_write_controller_cmd(uint8_t cmd)
{
    uint64_t deadline;
    int rc = i8042_deadline_new(&deadline);
    if (rc != I8042_OK)
        return rc;

    uint64_t flags = spin_lock_irqsave(&i8042_lock);
    rc = i8042_wait_ibf_empty(deadline);
    if (rc == I8042_OK)
        arch_outb(I8042_STATUS_PORT, cmd);
    spin_unlock_irqrestore(&i8042_lock, flags);
    return rc;
}

int i8042_read_command_byte(uint8_t *out)
{
    uint64_t deadline;
    int rc = i8042_deadline_new(&deadline);
    if (rc != I8042_OK)
        return rc;

    uint64_t flags = spin_lock_irqsave(&i8042_lock);

    // 1. Drain every pre-existing OBF byte with the NORMAL demux rules:
    //    keyboard bytes reach their consumer, stale aux bytes are dropped
    //    when no aux consumer exists. Only proceed on a confirmed-empty
    //    output buffer so the response we accept below is provably NEW.
    rc = i8042_drain_all(deadline);
    if (rc != I8042_OK)
        goto out;

    // 2. Issue 0x20.
    rc = i8042_wait_ibf_empty(deadline);
    if (rc != I8042_OK)
        goto out;
    arch_outb(I8042_STATUS_PORT, I8042_CMD_READ_CB);

    // 3. Wait for this command's response. It carries no AUX flag and
    //    MUST bypass both consumers — reading 0x60 directly here is the
    //    only path; normal dispatch is never used for it.
    rc = i8042_wait_obf_full(deadline);
    if (rc != I8042_OK)
        goto out;                       // timeout: nothing written back
    *out = arch_inb(I8042_DATA_PORT);

out:
    spin_unlock_irqrestore(&i8042_lock, flags);
    return rc;
}

int i8042_write_command_byte(uint8_t v)
{
    uint64_t deadline;
    int rc = i8042_deadline_new(&deadline);
    if (rc != I8042_OK)
        return rc;

    uint64_t flags = spin_lock_irqsave(&i8042_lock);

    // 0x60 + value is one non-interruptible transaction.
    rc = i8042_wait_ibf_empty(deadline);
    if (rc != I8042_OK)
        goto out;
    arch_outb(I8042_STATUS_PORT, I8042_CMD_WRITE_CB);

    rc = i8042_wait_ibf_empty(deadline);
    if (rc != I8042_OK)
        goto out;
    arch_outb(I8042_DATA_PORT, v);

out:
    spin_unlock_irqrestore(&i8042_lock, flags);
    return rc;
}

int i8042_write_aux_byte(uint8_t data)
{
    uint64_t deadline;
    int rc = i8042_deadline_new(&deadline);
    if (rc != I8042_OK)
        return rc;

    uint64_t flags = spin_lock_irqsave(&i8042_lock);

    // 0xD4 + data is one non-interruptible transaction. The device ACK
    // (AUX-sourced 0xFA) is collected later via i8042_pump().
    rc = i8042_wait_ibf_empty(deadline);
    if (rc != I8042_OK)
        goto out;
    arch_outb(I8042_STATUS_PORT, I8042_CMD_AUX_WRITE);

    rc = i8042_wait_ibf_empty(deadline);
    if (rc != I8042_OK)
        goto out;
    arch_outb(I8042_DATA_PORT, data);

out:
    spin_unlock_irqrestore(&i8042_lock, flags);
    return rc;
}
