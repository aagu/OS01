/* Host shadow of <core/debug.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * debug_irq() compiles to nothing — the hosttest asserts on port
 * writes, not on log text. */
#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

#define debug_irq(fmt, ...) do {} while (0)

#endif /* _KERNEL_DEBUG_H */
