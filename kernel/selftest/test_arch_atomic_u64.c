// kernel/selftest/test_arch_atomic_u64.c
// Verify the arch_atomic_or_u64 / arch_atomic_and_u64 facade declared in
// <arch/atomic.h>. The facade is arch-neutral; per-arch strong overrides
// live in kernel/arch/<arch>/atomic.c (x86_64: lock orq / lock andq;
// aarch64: ldaxr + stlxr + cbnz LR/SC retry). selftest runs at the
// arch-neutral layer — no #ifdef __x86_64__ in this TU.
//
// Uses log_err (arch-neutral: log/log.c on x86_64, arch/aarch64/log_impl.c
// on aarch64) instead of serial_printk, which is x86_64-only.
#include <arch/atomic.h>
#include <core/selftest.h>
#include <log/log.h>
#include <stdint.h>

static uint64_t atomic_target;

int test_arch_atomic_u64_or_and(void) {
    atomic_target = 0;
    /* Set bit 3 atomically */
    arch_atomic_or_u64(&atomic_target, (uint64_t)1 << 3);
    if ((atomic_target & ((uint64_t)1 << 3)) == 0) {
        log_err("[selftest] arch_atomic_or_u64: FAIL\n");
        return -1;
    }
    /* Clear bit 3 atomically */
    arch_atomic_and_u64(&atomic_target, ~((uint64_t)1 << 3));
    if (atomic_target & ((uint64_t)1 << 3)) {
        log_err("[selftest] arch_atomic_and_u64: FAIL\n");
        return -1;
    }
    return 0;
}