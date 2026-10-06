#ifndef ARCH9_BACKEND_RUNTIME_H
#define ARCH9_BACKEND_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif

#ifndef debug_pci
#define debug_pci(...) do {} while (0)
#endif

#ifndef debug_irq
#define debug_irq(...) do {} while (0)
#endif

#ifndef log_info
#define log_info(...) do {} while (0)
#endif

#ifndef log_warn
#define log_warn(...) do {} while (0)
#endif

#ifndef log_err
#define log_err(...) do {} while (0)
#endif

#ifndef log_debug
#define log_debug(...) do {} while (0)
#endif

/* Real atomic spinlock implementation for host test concurrency */
static inline uint64_t backend_test_spin_lock_irqsave(spinlock_T *l)
{
    unsigned long expected = 1;
    while (!__atomic_compare_exchange_n(&l->lock, &expected, 0, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 1;
        #if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
        #endif
    }
    return 0;
}

static inline void backend_test_spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&l->lock, 1, __ATOMIC_RELEASE);
}

#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#define spin_lock_irqsave(l) backend_test_spin_lock_irqsave(l)
#define spin_unlock_irqrestore(l, f) backend_test_spin_unlock_irqrestore(l, f)

/* Mock I/O port hooks for CF8 / CFC observation */
typedef void (*mock_outd_fn)(uint16_t port, uint32_t value);
typedef uint32_t (*mock_ind_fn)(uint16_t port);

extern mock_outd_fn g_mock_outd;
extern mock_ind_fn  g_mock_ind;

static inline void arch_outd(uint16_t port, uint32_t value)
{
    if (g_mock_outd) {
        g_mock_outd(port, value);
    }
}

static inline uint32_t arch_ind(uint16_t port)
{
    if (g_mock_ind) {
        return g_mock_ind(port);
    }
    return 0;
}

#endif /* ARCH9_BACKEND_RUNTIME_H */
