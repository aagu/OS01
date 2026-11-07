#ifndef _DRIVER_PIT_H
#define _DRIVER_PIT_H

#include <stdint.h>
#include <stdbool.h>

#define CLOCK_FREQUENCY 1193180
#define PIT_COMMAND 0x43
#define PIT_DATA 0x40
#define PIT_ICW 0x36

/* BOOT-2: PIT prepare/start split (spec §5.1).
 *
 *   pit_init    prepare-only: register the handler with IRQF_NO_AUTOEN
 *               and program 100 Hz.  Never unmasks.  Returns 0 / -1.
 *   pit_start   one-shot: unmask the PIT route and let it tick.  The
 *               handoff's mask/unmask pairs are NOT second starts.
 *   pit_prepared query the prepare state.
 *
 * Repeat prepare/start is a boot-protocol error (the caller fatals).
 */
int pit_init(void);
int pit_start(void);
bool pit_prepared(void);

/* Legacy frequency programming (used by pit_init). */
void set_frequency(uint16_t hz);

#endif /* _DRIVER_PIT_H */
