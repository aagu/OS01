/* Host-only runtime surface for compiling the REAL kernel/driver/i8042.c
 * (PS/2 mouse driver Task 4).
 *
 * Build contract (see I8042_HOST_CFLAGS in hosttests/Makefile):
 *   -D_ARCH_IO_H       skips <arch/io.h>; arch_inb/arch_outb below are the
 *                      MOCK port model, defined once in test_i8042_demux.c
 *                      so the production object and the test TU share state.
 *   -D_ARCH_SPINLOCK_H skips <arch/spinlock.h>; host spinlock below tracks
 *                      hold depth so tests can assert consumers run in-lock.
 *   -D_ARCH_CPU_H      skips <arch/cpu.h>; arch_cpu_pause is a no-op.
 *
 * clocksource_cycles()/clocksource_freq_hz() are declared by i8042.c itself
 * (deliberately NOT via <time/clocksource.h>, which pulls scheduler/percpu
 * headers unusable before GS base install); the test TU defines them over a
 * controllable virtual clock.
 */
#ifndef OS01_I8042_TEST_RUNTIME_H
#define OS01_I8042_TEST_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* ── Mock port I/O (definitions live in test_i8042_demux.c) ── */
uint8_t arch_inb(uint16_t port);
void    arch_outb(uint16_t port, uint8_t val);

/* ── Host spinlock with hold-depth tracking ───────────────────
 * Single-threaded host tests: "holding" is just a counter the test
 * asserts on (consumers must observe depth > 0). */
extern int i8042_mock_lock_depth;

typedef struct { unsigned long lock; } spinlock_T;

static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    (void)l;
    i8042_mock_lock_depth++;
    return 0x200; /* pretend RFLAGS.IF was set */
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)l;
    (void)flags;
    i8042_mock_lock_depth--;
}

static inline void arch_cpu_pause(void) {}

#endif /* OS01_I8042_TEST_RUNTIME_H */
