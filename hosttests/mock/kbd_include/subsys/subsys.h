/* Host shadow of <subsys/subsys.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * SUBSYS_INITCALL becomes a plain static pointer + ELF constructor so
 * the test binary actually runs _keyboard_register at startup; the
 * mocked register_subsys (test_i8042_demux.c) records the (name,
 * init-wrapper, phase, flags) tuple for assertions — including that
 * the wrapper propagates keyboard_init()'s return status. */
#ifndef _KERNEL_SUBSYS_H
#define _KERNEL_SUBSYS_H

#include <stdint.h>

#define SUBSYS_PHASE_5        5
#define SUBSYS_FLAG_OPTIONAL  (1 << 0)

typedef int (*subsys_initcall_t)(void);

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags);

/* Host-test hook: receives each SUBSYS_INITCALL function pointer. */
void kbd_mock_subsys_register_initcall(int (*fn)(void));

#define SUBSYS_INITCALL(fn)                                             \
    static subsys_initcall_t __subsys_initcall_##fn                    \
        __attribute__((used)) = fn;                                     \
    __attribute__((constructor))                                        \
    static void __subsys_ctor_##fn(void)                                \
    {                                                                   \
        kbd_mock_subsys_register_initcall(__subsys_initcall_##fn);     \
    }

#endif /* _KERNEL_SUBSYS_H */
