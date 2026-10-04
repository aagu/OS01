#ifndef MOCK_ARCH_SPINLOCK_H
#define MOCK_ARCH_SPINLOCK_H

/* Shadow of kernel/include/arch/spinlock.h for the
 * test_aarch64_color_printk_abi host test. core/printk.h only needs the
 * spinlock_T type for its `position` struct; test_platform.h (force-
 * included by FRAMEWORK_INC) already provides that type, and the real
 * arch/x86_64/spinlock.h inline-asm machinery does not host-compile
 * inside this TU. Deliberately empty. */

#endif
