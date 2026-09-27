/* Host shadow of <percpu/percpu.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * keyboard.c only uses this_cpu()->need_resched and the num_cpus
 * BSP-GS-readiness gate.  The mock this_cpu() counts calls (defined in
 * test_i8042_demux.c) so tests can assert it is NOT touched before
 * num_cpus != 0 (GS base not installed on the BSP yet). */
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H

#include <stdint.h>

typedef struct {
    volatile int need_resched;
} percpu_t;

/* Written by kernel_main only AFTER percpu_install_gs(0) — the BSP
 * GS-readiness gate used by keyboard_wake_pollers. */
extern uint32_t num_cpus;

/* Call counter — assertions use it to prove GS-free early wakes. */
extern int kbd_mock_this_cpu_calls;

percpu_t *this_cpu(void);

#endif /* _KERNEL_PERCPU_H */
