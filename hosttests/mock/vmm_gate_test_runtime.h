/* hosttests/mock/vmm_gate_test_runtime.h
 *
 * Host-only runtime surface for compiling the PRODUCTION
 * kernel/arch/aarch64/memory/vmm_gate.c on the host (M3 Task 7 Step 4).
 *
 * Pattern mirrors hosttests/mock/clocksource_test_runtime.h: short-circuit
 * the heavy kernel headers with `#define _XXX_H` BEFORE they are included,
 * then provide host-friendly stubs for the symbols vmm_gate.c needs.
 *
 * IMPORTANT: this header MUST be -included BEFORE any kernel header. The
 * Makefile rule for vmm_gate_production.o does NOT include test_platform.h,
 * so this header owns the entire stub surface for that TU.
 *
 * Symbols supplied by the test TU (not here):
 *   - percpu_data[8]                 (defined in the test case file)
 *   - dtb_cpu_count()                (mock, backed by a settable variable)
 *   - vmm_gate_violation() override  (longjmp hook; overrides the weak
 *                                     default in vmm_gate.c)
 */
#ifndef OS01_VMM_GATE_TEST_RUNTIME_H
#define OS01_VMM_GATE_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NR_CPUS 8

/* Short-circuit the heavy kernel headers vmm_gate.c pulls in. */
#define _KERNEL_PERCPU_H
#define _ARCH_SPINLOCK_H

/* Minimal percpu tail: only the M3 shootdown fields vmm_gate.c touches.
 * Layout of the preceding fields is irrelevant to the hosttest; the
 * production layout is pinned by _Static_asserts in percpu.h / percpu.c. */
typedef struct percpu {
    uint64_t self;
    uint64_t need_resched;
    uint32_t ipi_ready;        /* published once via ipi_ready_publish_and_count */
    uint32_t tlb_ack_gen;
} percpu_t;

extern percpu_t percpu_data[NR_CPUS];

/* No-op spinlock (single-threaded host test). Name must match the
 * production <arch/spinlock.h> surface used by vmm_gate.c. */
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }

#endif /* OS01_VMM_GATE_TEST_RUNTIME_H */
