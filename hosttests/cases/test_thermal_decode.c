/*
 * hosttests/cases/test_thermal_decode.c
 *
 * Thermal plan Task 3 — host-side decoder fixtures for the pure
 * IA32_THERM_STATUS decode path (kernel/arch/x86_64/cpu/thermal.c).
 *
 * Backend probe (vendor/hypervisor/DTS/whitelist) and worker
 * lifecycle are kernel-only and live in the in-kernel selftest
 * (kernel/selftest/test_thermal_decode.c).  Here we test the
 * deterministic decode math plus the symbol/linkage contract that
 * the strong backend publishes.
 *
 * Run via:
 *   make PROFILE=x86_64-clang test-thermal-host CASE=decode
 */

#include "test_framework.h"
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <arch/thermal.h>

/* Local spinlock_T stub — <sched/task.h> transitively references it
 * via inline thread->spin members; keep parity with the affinity
 * fixture's local typedef so the host build keeps compiling under
 * -D__SPINLOCK_H__. */
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)l; (void)f; }
static inline long spin_trylock(spinlock_T *l) { (void)l; return 1; }

/* ── Direct decode tests against the production function symbol ── */

static int test_decode_target_100_distance_25(void)
{
    int32_t t = 0;
    /* target=100, reading=25 (bits 22:16), valid=1 (bit 31).
     * Decoder returns (target - reading) * 1000 = 75_000. */
    uint64_t status = (1U << 31) | (25U << 16);
    int rc = thermal_decode_core(100, status, &t);
    if (rc != 0)        { printf("rc=%d\n", rc); return 1; }
    if (t != 75000)     { printf("t=%d\n", (int)t); return 1; }
    return 0;
}

static int test_decode_target_100_distance_110(void)
{
    int32_t t = 0;
    /* target=100, reading=110 → (100 - 110) * 1000 = -10_000. */
    uint64_t status = (1U << 31) | (110U << 16);
    int rc = thermal_decode_core(100, status, &t);
    if (rc != 0)        { printf("rc=%d\n", rc); return 1; }
    if (t != -10000)    { printf("t=%d\n", (int)t); return 1; }
    return 0;
}

static int test_decode_target_0_valid_0(void)
{
    int32_t t = 0;
    uint64_t status = 0;  /* no valid bit set, no reading */
    int rc = thermal_decode_core(0, status, &t);
    if (rc != -EINVAL) return 1;
    return 0;
}

static int test_decode_target_0_valid_1_zero_reading(void)
{
    /* valid=1 but reading=0 → INVALID per the plan's decode rule
     * (reading==0 means "chip reports no current temperature"). */
    int32_t t = 0;
    uint64_t status = (1U << 31);   /* valid=1, reading=0 */
    int rc = thermal_decode_core(0, status, &t);
    if (rc != -EINVAL) {
        printf("decode.valid_no_reading: rc=%d\n", rc);
        return 1;
    }
    return 0;
}

static int test_decode_negative_distance(void)
{
    /* target=100, reading=5 → temp=95°C → 95_000 mC. */
    int32_t t = 0;
    uint64_t status = (1U << 31) | (5U << 16);
    int rc = thermal_decode_core(100, status, &t);
    if (rc != 0)        return 1;
    if (t != 95000)     { printf("t=%d\n", (int)t); return 1; }
    return 0;
}

static int test_decode_null_output(void)
{
    int rc = thermal_decode_core(100, (1U<<31)|(100U<<16), NULL);
    if (rc != -EINVAL) return 1;
    return 0;
}

/* ── Reason/state enum contract ────────────────────────────
 * The fixed transport enum values must be stable — procfs_fmt.c
 * reads them by value. */

static int test_enum_reason_distinct(void)
{
    /* Distinct enum values (no accidental collisions).  Spot-check
     * a few we know must be different — if a future patch collapses
     * them, the procfs string table silently prints wrong strings. */
    if (THERMAL_REASON_NONE == THERMAL_REASON_HYPERVISOR) return 1;
    if (THERMAL_REASON_VENDOR == THERMAL_REASON_NO_DTS)    return 1;
    if (THERMAL_REASON_MODEL == THERMAL_REASON_NOT_READY)  return 1;
    if (THERMAL_REASON_MSR == THERMAL_REASON_READING)      return 1;
    if (THERMAL_REASON_TARGET == THERMAL_REASON_WORKER_START) return 1;
    return 0;
}

static int test_enum_state_distinct(void)
{
    if (THERMAL_OK == THERMAL_UNSUPPORTED)           return 1;
    if (THERMAL_OK == THERMAL_INVALID)               return 1;
    if (THERMAL_OK == THERMAL_READ_ERROR)            return 1;
    if (THERMAL_STALE == THERMAL_UNSUPPORTED)        return 1;
    return 0;
}

/* ── struct layout sanity ─────────────────────────────────── */
static int test_sample_layout(void)
{
    /* temp_mc is int32_t and comes first so a reader that
     * over-reads by 4 bytes doesn't see a state out of context. */
    if (offsetof(struct thermal_sample, temp_mc) != 0) return 1;
    if (sizeof(((struct thermal_sample *)0)->temp_mc) != sizeof(int32_t))
        return 1;
    return 0;
}

int main(void)
{
    int total = 0, fail = 0;
    printf("\n  == thermal_decode host tests ==\n");

#define T(name) do {                                         \
        total++;                                            \
        if (name() == 0) printf("  PASS %s\n", #name);      \
        else { fail++; printf("  FAIL %s\n", #name); }      \
    } while (0)

    T(test_decode_target_100_distance_25);
    T(test_decode_target_100_distance_110);
    T(test_decode_target_0_valid_0);
    T(test_decode_target_0_valid_1_zero_reading);
    T(test_decode_negative_distance);
    T(test_decode_null_output);
    T(test_enum_reason_distinct);
    T(test_enum_state_distinct);
    T(test_sample_layout);

#undef T

    printf("  == thermal_decode summary: %d/%d passed, %d failed ==\n",
           total - fail, total, fail);
    return fail ? 1 : 0;
}
