#ifndef _DRIVER_I8042_H
#define _DRIVER_I8042_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Origin of a byte popped from the 8042 output buffer, as reported by
 * status register bit 5 at the moment the byte was read. */
typedef enum {
    I8042_SRC_NONE = 0,
    I8042_SRC_KBD,       /* bit5 = 0 — keyboard port */
    I8042_SRC_AUX,       /* bit5 = 1 — aux (mouse) port */
} i8042_src_t;

/* Return codes: 0 = success, negative = failure (no errno — this runs
 * early in boot with a shallow stack, phase 5/6). */
#define I8042_OK           0
#define I8042_ERR_TIMEOUT  (-1)   /* IBF/OBF wait exceeded its deadline   */
#define I8042_ERR_NO_CLOCK (-2)   /* clocksource_freq_hz() == 0          */

/* Consumes one already-demultiplexed byte. Called with the i8042 lock
 * HELD and local IRQs disabled: must not touch the i8042 ports, take the
 * i8042 lock, sleep, or copy user memory. */
typedef void (*i8042_consumer_t)(uint8_t byte);

/* Initialize the shared controller lock and demux state. Call once,
 * before the first port access (keyboard_init does so at SUBSYS_PHASE_5).
 * Idempotent. */
void i8042_init(void);

/* Install the keyboard / aux byte sinks. NULL = bytes of that source are
 * dropped (used to drain stale aux bytes before the mouse exists). */
void i8042_set_kbd_consumer(i8042_consumer_t consumer);
void i8042_set_aux_consumer(i8042_consumer_t consumer);

/* Drain pending output-buffer bytes: read status + data as PAIRED reads
 * inside one spin_lock_irqsave(i8042_lock) critical section and dispatch
 * by the AUX bit (bit 5). Never reads 0x60 when OBF (bit 0) is clear.
 * The IRQ line that triggered the call is only a wake-up hint — actual
 * ownership is decided per byte by the status register.
 * Returns I8042_OK when the buffer drained empty, or I8042_ERR_TIMEOUT
 * if a per-call byte cap (32) was reached with OBF still asserted — a
 * stuck status line must never spin forever in-lock with IRQs off. */
int i8042_pump(void);

/* Write a controller command to 0x64, waiting (bounded) for IBF empty. */
int i8042_write_controller_cmd(uint8_t cmd);

/* 0x20 transaction: drain all pre-existing OBF bytes via the normal demux
 * rules (kbd bytes dispatched, aux bytes dropped when no consumer), only
 * then issue 0x20 and wait for THIS command's response. The response has
 * no AUX flag and is returned via *out — it is never dispatched to the
 * keyboard consumer or the aux packet parser. Runs in one locked,
 * local-IRQs-off critical section under a single overall deadline; on
 * timeout nothing is written back and a negative value is returned. */
int i8042_read_command_byte(uint8_t *out);

/* 0x60 transaction: write command 0x60 to 0x64 followed by the value to
 * 0x60 as one non-interruptible pair. */
int i8042_write_command_byte(uint8_t v);

/* Aux-port transaction: 0xD4 to 0x64 followed by the data byte to 0x60 as
 * one non-interruptible pair. Does NOT wait for the device ACK — the
 * caller polls it through i8042_pump(). */
int i8042_write_aux_byte(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* _DRIVER_I8042_H */
