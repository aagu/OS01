/* test/mock/slab_include/percpu/percpu.h — host shadow of
 * kernel/include/percpu/percpu.h for compiling the REAL
 * kernel/memory/slab.c in the host test suite (test_slab_idempotent_reservation,
 * test_slab_basic_x86_count).
 *
 * The production header transitively pulls in <sched/task.h>
 * (struct task_struct, struct mm_struct, all of fs/file.h, ...) and
 * <arch/spinlock.h> / <arch/percpu.h> (inline asm reading GS:0 /
 * TPIDR_EL1). All of those are unreachable from a host process.
 *
 * The only symbols slab.c consumes from the percpu surface are:
 *   - percpu_t  (with the `online` field — slab.c branches on
 *                percpu_data[0].online to skip locking in early boot)
 *   - percpu_t percpu_data[NR_CPUS]
 *   - uint32_t cpu_id(void)
 *
 * This shadow provides exactly that minimal surface. The host test
 * controls `online` via the fixture (default false; tests that need
 * the locking branch flip it on).
 *
 * This directory is placed BEFORE kernel/include on the include path
 * (SLAB_SHADOW_INC in test/Makefile) so only this header is shadowed;
 * every other <kernel/...> include resolves to the production header.
 */
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H

#include <stdint.h>

#define NR_CPUS 8

/* Mirrors the production percpu_t field order at offset 8..28
 * (self, need_resched, cpu_id, online, ...) so any asm-style access
 * patterns happen to work; slab.c only reads `online`. */
typedef struct percpu {
    uint64_t self;
    uint64_t need_resched;
    uint32_t cpu_id;
    uint32_t arch_processor_id;
    uint32_t online;
    uint32_t scheduler_ok;
    /* rest of the struct irrelevant to slab.c — pad to a safe size. */
    char _pad[144 - 8 - 8 - 4 - 4 - 4 - 4];
} percpu_t;

/* Storage definition (not just `extern`): the host shadow header is
 * only ever included by the slab test TUs (slab_production.o +
 * test_slab_idempotent_reservation.o + test_slab_basic_x86_count.o),
 * and a single definition across all of them satisfies the link. */
percpu_t percpu_data[NR_CPUS];

/* slab.c only calls cpu_id() under percpu_data[0].online, so the
 * stub return value doesn't matter for the suite's invariants. */
static inline uint32_t cpu_id(void) { return 0; }

#endif /* _KERNEL_PERCPU_H */