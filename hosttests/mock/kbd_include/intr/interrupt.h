/* Host shadow of <intr/interrupt.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * Only the surface keyboard.c touches is provided: the pt_regs_t
 * typedef, the IRQ trigger flag used by register_irq(1, ...), and the
 * register_irq declaration itself (mocked in test_i8042_demux.c with a
 * scriptable return code). */
#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>

#define IRQF_TRIGGER_EDGE 0x01

typedef struct pt_regs pt_regs_t;

int32_t register_irq(uint32_t gsi, void *arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t *regs),
        uint64_t parameter, uint32_t flags, const char *irq_name);

#endif /* _KERNEL_INTERRUPT_H */
