#ifndef MOCK_DRIVER_SERIAL_H
#define MOCK_DRIVER_SERIAL_H

/* Shadow of kernel/include/driver/serial.h for the
 * test_aarch64_color_printk_abi host test.
 *
 * The production header pulls in <tty/tty.h> and the arch spinlock
 * machinery; this harness only needs the two symbols that
 * kernel/core/printk.c's serial path touches: serial_lock and
 * write_serial_unlocked.  write_serial_unlocked is defined by
 * printk_capture.c, which records the emitted bytes so the test can
 * assert the production formatter's output.  spinlock_T is provided by
 * the force-included test_platform.h (FRAMEWORK_INC). */

#include <arch/spinlock.h>   /* empty shadow; spinlock_T from test_platform.h */

extern spinlock_T serial_lock;
void write_serial_unlocked(char c);

#endif /* MOCK_DRIVER_SERIAL_H */
