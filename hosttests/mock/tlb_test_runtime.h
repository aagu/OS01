/* hosttests/mock/tlb_test_runtime.h
 *
 * Host-only runtime surface for compiling the PRODUCTION
 * kernel/memory/tlb.c on the host (M2/M3 Task 12 Step 1).
 *
 * Pattern mirrors mock/vmm_gate_test_runtime.h: short-circuit the heavy
 * kernel headers by defining their include guards BEFORE they are
 * included, then provide host-friendly stubs for the symbols tlb.c
 * needs.  MUST be -included BEFORE any kernel header.
 *
 * Symbols supplied by the test TU (not here):
 *   - percpu_data[NR_CPUS], num_cpus     (test-controlled state)
 *   - tlb_shootdown_panic() override     (longjmp capture; overrides the
 *                                         weak default in tlb.c)
 *   - mock ipi_broadcast()               (records the last mask/vector)
 *   - mock arch_flush_tlb_all()          (records local flush count)
 *
 * Test-controllable knobs defined here (shared state, declared in the
 * header so both the production TU and the test TU see the same object):
 *   - tlb_mock_cycles: fake arch_cycle_counter() value; the test bumps
 *     it past the deadline to drive the timeout path deterministically.
 */
#ifndef OS01_TLB_TEST_RUNTIME_H
#define OS01_TLB_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NR_CPUS 8

/* Short-circuit the heavy kernel headers tlb.c pulls in. */
#define _KERNEL_PERCPU_H
#define _ARCH_SPINLOCK_H
#define __SPINLOCK_H__
#define _ARCH_MMU_H
#define _ARCH_CPU_H
#define _KERNEL_IPI_H
#define _KERNEL_DEBUG_H

/* Minimal percpu_t: only the fields tlb.c touches.  The production
 * layout is pinned by test_percpu_layout.c. */
typedef struct percpu {
    uint64_t self;
    uint64_t need_resched;
    uint32_t cpu_id;
    uint32_t arch_processor_id;
    uint32_t online;
    uint32_t ipi_ready;
    uint32_t tlb_ack_gen;
} percpu_t;

extern percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;

static inline percpu_t *this_cpu(void) { return &percpu_data[0]; }
static inline uint32_t cpu_id(void)     { return percpu_data[0].cpu_id; }

/* No-op spinlock (single-threaded host test). */
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }

/* Mock cycle counter: the test advances it to drive the timeout.  Each
 * arch_cpu_pause() (the production wait loop's yield) jumps the clock
 * by 1e8 cycles so the 1e9-cycle deadline (TLB_SD_TIMEOUT_CYCLES) is
 * exhausted in ~10 loop iterations on the host. */
extern uint64_t tlb_mock_cycles;
static inline uint64_t arch_cycle_counter(void) { return tlb_mock_cycles; }
static inline void arch_cpu_pause(void) { tlb_mock_cycles += 100000000ULL; }

/* ipi.h surface (header short-circuited above).  ipi_broadcast itself is
 * the test TU's mock — tlb.c only calls it. */
#define IPI_VECTOR_TLB      0x40
#define IPI_VECTOR_RESCHED  0x41
void ipi_broadcast(uint32_t vector, uint64_t target_mask);

/* arch_flush_tlb_all: recorded by the test TU. */
extern uint32_t tlb_mock_local_flushes;
static inline void arch_flush_tlb_all(void) { tlb_mock_local_flushes++; }

#endif /* OS01_TLB_TEST_RUNTIME_H */
