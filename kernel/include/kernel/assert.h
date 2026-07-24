#ifndef _KERNEL_ASSERT_H
#define _KERNEL_ASSERT_H

#include <kernel/log.h>

#define ASSERT(x) do { \
    if (!(x)) { \
        log_err("ASSERT failed: %s at %s:%d\n", #x, __FILE__, __LINE__); \
        while (1) { __asm__ __volatile__("hlt"); } \
    } \
} while (0)

#endif /* _KERNEL_ASSERT_H */
