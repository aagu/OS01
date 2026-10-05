/* hosttests/mock/arch9/ahci_runtime.h — mock environment for AHCI lifecycle tests */
#ifndef ARCH9_AHCI_RUNTIME_H
#define ARCH9_AHCI_RUNTIME_H

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

#ifndef arch_nop
#define arch_nop() do {} while (0)
#endif

/* Atomic spinlock for host tests */
static inline uint64_t ahci_test_spin_lock_irqsave(spinlock_T *l)
{
    unsigned long expected = 1;
    while (!__atomic_compare_exchange_n(&l->lock, &expected, 0, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 1;
    }
    return 0;
}

static inline void ahci_test_spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&l->lock, 1, __ATOMIC_RELEASE);
}

#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#define spin_lock_irqsave(l) ahci_test_spin_lock_irqsave(l)
#define spin_unlock_irqrestore(l, f) ahci_test_spin_unlock_irqrestore(l, f)

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

/* PMM / 2MB Huge page simulation */
#define ZONE_NORMAL 0
#define PAGE_KERNEL_PMD_NOCACHE 0

struct Page {
    uint64_t phy_address;
    void *virt_address;
};

struct Page *alloc_pages(int zone, int order, int flags);
void free_pages(struct Page *page, int order);
struct Page *Virt_To_Page(void *virt);

static inline void vmm_map_page(void *map, uint64_t paddr, uintptr_t vaddr, uint64_t flags)
{
    (void)map; (void)paddr; (void)vaddr; (void)flags;
}

static inline void flush_tlb(void) {}

#define kernel_map NULL

/* Percpu and clocksource */
extern percpu_t g_ahci_percpu;
#undef this_cpu
static inline percpu_t *ahci_this_cpu(void) { return &g_ahci_percpu; }
#define this_cpu() ahci_this_cpu()

extern bool     clocksource_active;
extern uint32_t clocksource_mult;
extern uint32_t clocksource_shift;
extern volatile uint64_t jiffies;

uint64_t clocksource_freq_hz(void);
uint64_t clocksource_cycles(void);
uint64_t clocksource_read_ns(void);
uint64_t arch_cycle_counter(void);

#ifndef _ARCH_IO_H
#define _ARCH_IO_H 1
#endif
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H 1
#endif
#ifndef _KERNEL_ARCH_X86_64_CPU_H
#define _KERNEL_ARCH_X86_64_CPU_H 1
#endif

uint32_t unregister_irq(uint32_t gsi);

/* Mock boundary counters */
extern uint32_t fake_mmio_write_count;
extern uint32_t fake_dma_start_count;
extern uint32_t fake_bounce_write_count;
extern uint32_t fake_free_pages_count;
extern uint32_t fake_quarantine_count;
extern uint32_t fake_irq_free_count;

/* Simulation flags */
extern bool s_simulate_command_timeout;
extern bool s_simulate_stop_timeout;
extern bool s_simulate_interrupt_disable_fail;
extern bool s_simulate_identify_fail;

void advance_cycles(uint64_t delta);
#undef arch_nop
#define arch_nop() do { advance_cycles(10000000ULL); } while (0)

void mock_ahci_write32(volatile uint32_t *reg, uint32_t val);
#undef ahci_write32
#define ahci_write32(reg, val) mock_ahci_write32((volatile uint32_t *)(reg), (uint32_t)(val))

struct pci_device;
int mock_pci_interrupts_disable(struct pci_device *pdev);
#define pci_interrupts_disable(pdev) mock_pci_interrupts_disable(pdev)

#define ahci_record_dma_start() do { \
    fake_dma_start_count++; \
} while (0)

#define ahci_record_bounce_write() do { \
    fake_bounce_write_count++; \
} while (0)

#endif /* ARCH9_AHCI_RUNTIME_H */
