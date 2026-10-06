/* hosttests/mock/arch9/e1000_runtime.h — mock environment for e1000 instance tests */
#ifndef ARCH9_E1000_RUNTIME_H
#define ARCH9_E1000_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif
#ifndef debug_net
#define debug_net(...) do {} while (0)
#endif
#ifndef debug_pci
#define debug_pci(...) do {} while (0)
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

#include "net_runtime.h"

/* Atomic spinlock for host tests */
static inline uint64_t e1000_test_spin_lock_irqsave(spinlock_T *l)
{
    unsigned long expected = 1;
    while (!__atomic_compare_exchange_n(&l->lock, &expected, 0, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 1;
    }
    return 0;
}

static inline void e1000_test_spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&l->lock, 1, __ATOMIC_RELEASE);
}

#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#define spin_lock_irqsave(l) e1000_test_spin_lock_irqsave(l)
#define spin_unlock_irqrestore(l, f) e1000_test_spin_unlock_irqrestore(l, f)

#ifndef spin_lock_init
#define spin_lock_init(l) do { (l)->lock = 1; } while (0)
#endif
#ifndef spin_init
#define spin_init(l) do { (l)->lock = 1; } while (0)
#endif

#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif
#ifndef _ARCH_X86_64_SPINLOCK_H
#define _ARCH_X86_64_SPINLOCK_H 1
#endif

#ifndef _ARCH_IO_H
#define _ARCH_IO_H 1
#endif
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H 1
#endif
#ifndef _KERNEL_ARCH_X86_64_CPU_H
#define _KERNEL_ARCH_X86_64_CPU_H 1
#endif

#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H 1
#endif
#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H 1
#endif
#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H 1
#endif

#undef Phy_To_Virt
#define Phy_To_Virt(x) ((void*)(uintptr_t)(x))

#ifndef arch_wmb
#define arch_wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

#define ZONE_NORMAL 0
#define PAGE_KERNEL_PMD_NOCACHE 0
#define PAGE_2M_MASK (~(0x200000ULL - 1))
#define kernel_map NULL

static inline void vmm_map_page(void *map, uint64_t paddr, uintptr_t vaddr, uint64_t flags)
{
    (void)map; (void)paddr; (void)vaddr; (void)flags;
}

static inline void flush_tlb(void) {}

/* Page structs and simulated DMA allocation */
struct Page {
    uint64_t phy_address;
    void *virt_address;
};

extern uint32_t fake_free_pages_count;
extern uint32_t fake_free_4k_count;
extern uint32_t fake_alloc_pages_count;
extern uint32_t fake_alloc_4k_count;
extern bool s_inject_alloc_pages_fail;
extern int s_inject_alloc_4k_fail_after;
extern bool s_inject_net_register_fail;

struct Page *alloc_pages(int zone, int count, int flags);
void free_pages(struct Page *page, int count);
uint64_t alloc_4k_page(void);
void free_4k_page(uint64_t phys);

/* lwIP helpers */
#ifndef pbuf_alloc
#define pbuf_alloc(layer, len, type) fake_pbuf_alloc(len)
#endif

#ifndef pbuf_take
static inline err_t pbuf_take(struct pbuf *p, const void *dataptr, uint16_t len)
{
    if (!p || !dataptr || len > p->len) return ERR_ARG;
    memcpy(p->payload, dataptr, len);
    return ERR_OK;
}
#endif

#define PBUF_RAW 0
#define PBUF_POOL 0

/* Wake counter */
extern uint32_t fake_sys_mbox_wake_count;
void sys_mbox_wake(void);

extern uint32_t fake_mmio_write_count;

#endif /* ARCH9_E1000_RUNTIME_H */
