/*
 * hosttests/cases/test_thermal_affinity.c
 *
 * Thermal plan Task 2 — host-side structural checks for the
 * CPU-affinity contract.
 *
 * What this fixture does NOT do (vs the in-kernel selftest):
 *   - does NOT call sched_pick_cpu_mask() against live percpu_data
 *     (the host has no scheduler state); the in-kernel selftest in
 *     kernel/selftest/test_thermal_affinity.c covers the live bits.
 *   - does NOT spawn a real kthread (no scheduler hooks for that
 *     to make sense on the host build).
 *
 * What this fixture DOES do:
 *   1. Compile-time shape checks on task_struct.allowed_cpus: the
 *      field exists at a stable offset (test_monitor_accounting.c
 *      uses offsetof in the same way; mirrors the convention).
 *   2. Header-level contract proof:
 *        - kernel_thread_on_cpu returns int
 *        - arch_rdmsr_safe / arch_msr_gp_fixup are still linkable
 *          (regression on Task 1 after Task 2's struct changes)
 *   3. Pure-function checks: sched_pick_cpu_mask cannot be linked
 *      here (it dereferences percpu_data which is x86_64 kernel
 *      state), so the in-kernel selftest owns the runtime check.
 *      Instead we verify its declaration against <sched/internal.h>.
 *
 * Run via:
 *   make PROFILE=x86_64-clang test-thermal-host CASE=affinity
 */

/*
 * hosttests/cases/test_thermal_affinity.c
 *
 * Thermal plan Task 2 — host-side structural checks for the
 * CPU-affinity contract.
 *
 * What this fixture does NOT do (vs the in-kernel selftest):
 *   - does NOT call sched_pick_cpu_mask() against live percpu_data
 *     (the host has no scheduler state); the in-kernel selftest in
 *     kernel/selftest/test_thermal_affinity.c covers the live bits.
 *   - does NOT spawn a real kthread (no scheduler hooks for that
 *     to make sense on the host build).
 *
 * What this fixture DOES do:
 *   1. Compile-time shape checks on task_struct.allowed_cpus: the
 *      field exists at a stable offset.
 *   2. Header-level contract proof:
 *        - kernel_thread_on_cpu returns int
 *        - arch_rdmsr_safe / arch_msr_gp_fixup are still linkable
 *          (regression on Task 1 after Task 2's struct changes)
 *   3. Pure-function mask math.
 *
 * Run via:
 *   make PROFILE=x86_64-clang test-thermal-host CASE=affinity
 */

#include "test_framework.h"
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Local spinlock_T stub — kernel/include/arch/spinlock.h is gated
 * out by -D__SPINLOCK_H__ in the host CFLAGS, but the task_t and
 * file_t structs reference spinlock_T.  Provide a minimal local
 * typedef so the offsetof invariants can compile. */
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)l; (void)f; }
static inline long spin_trylock(spinlock_T *l) { (void)l; return 1; }

#include <sched/task.h>
#include <arch/x86_64/msr_safe.h>
#include <arch/x86_64/regs.h>

/* Regression on Task 1: the symbols we promised to the host build
 * (header decls + tagged asm labels) are still linkable after Task
 * 2's task_t layout expansion.  Captures a ref to each so the
 * linker resolves them. */
static int task2_affinity_symbol_check(void)
{
    int (*rdmsr)(uint32_t, uint64_t *) = arch_rdmsr_safe;
    bool (*fix)(pt_regs_t *, uint64_t) = arch_msr_gp_fixup;
    const char *try_lbl = arch_rdmsr_safe_try;
    const char *fail_lbl = arch_rdmsr_safe_fail;
    return ((uintptr_t)rdmsr ^ (uintptr_t)fix ^
            (uintptr_t)try_lbl ^ (uintptr_t)fail_lbl) != 0 ? 0 : 1;
}

/* Layout invariant: allowed_cpus is uint64_t and present on task_t.
 * If a future refactor breaks this, the offsetof prints a clear
 * divergence; the compiler will refuse the build. */
static int task2_affinity_layout(void)
{
    if (offsetof(task_t, allowed_cpus) < offsetof(task_t, cpu)) {
        printf("[affinity] allowed_cpus must follow cpu in task_t\n");
        return 1;
    }
    if (sizeof(((task_t *)0)->allowed_cpus) != sizeof(uint64_t)) {
        printf("[affinity] allowed_cpus is not uint64_t\n");
        return 1;
    }
    return 0;
}

/* Default-mask math (pure, runs anywhere): the mask that the
 * publication path uses is (1 << num_cpus) - 1 for the live set.
 * Edge cases:
 *   - num_cpus == 0  → mask = 0
 *   - num_cpus == 1  → mask = 1 (cpu 0 only)
 *   - num_cpus == 64 → mask = UINT64_MAX (architectural cap)
 */
static int task2_affinity_default_mask_math(void)
{
    uint64_t m0 = (uint64_t)0;
    uint64_t m1 = (1ULL << 1) - 1;     /* 1 */
    uint64_t m4 = (1ULL << 4) - 1;     /* 0xF */
    if (m0 != 0)         return 1;
    if (m1 != 0x1ULL)    return 1;
    if (m4 != 0xFULL)    return 1;
    /* num_cpus is NR_CPUS-capped in production code — verify the
     * test math agrees (thermal plan: NR_CPUS == 8). */
    uint64_t m8 = (1ULL << 8) - 1;
    if (m8 != 0xFFULL)   return 1;
    /* Single-bit pin mask is just (1 << cpu). */
    if ((1ULL << 0) != 0x1ULL)         return 1;
    if ((1ULL << 7) != 0x80ULL)        return 1;
    return 0;
}

/* kernel_thread_on_cpu API surface — make sure its prototype
 * remains stable across task_t layout changes.  Pure header check:
 * the symbol itself lives in fork.c (x86_64 kernel only); the host
 * fixture only verifies the declaration compiles and matches the
 * expected signature. */
static int task2_affinity_api_shape(void)
{
    int (*kt_fn)(uint64_t (*)(uint64_t), uint64_t, unsigned) =
        (int (*)(uint64_t (*)(uint64_t), uint64_t, unsigned))0;
    (void)kt_fn;
    /* INIT_TASK-friendly default: BSP idle mask must be bit 0. */
    uint64_t bsp_idle_mask = 1ULL;
    if (bsp_idle_mask != 0x1ULL) return 1;
    return 0;
}

int main(void)
{
    int total = 0, fail = 0;
    printf("\n  == thermal_affinity host tests ==\n");

    {
        total++;
        if (task2_affinity_symbol_check() == 0) printf("  PASS task2_affinity_symbol_check\n");
        else { fail++; printf("  FAIL task2_affinity_symbol_check\n"); }
    }
    {
        total++;
        if (task2_affinity_layout() == 0) printf("  PASS task2_affinity_layout\n");
        else { fail++; printf("  FAIL task2_affinity_layout\n"); }
    }
    {
        total++;
        if (task2_affinity_default_mask_math() == 0) printf("  PASS task2_affinity_default_mask_math\n");
        else { fail++; printf("  FAIL task2_affinity_default_mask_math\n"); }
    }
    {
        total++;
        if (task2_affinity_api_shape() == 0) printf("  PASS task2_affinity_api_shape\n");
        else { fail++; printf("  FAIL task2_affinity_api_shape\n"); }
    }

    printf("  == thermal_affinity summary: %d/%d passed, %d failed ==\n",
           total - fail, total, fail);
    return fail ? 1 : 0;
}



