// kernel/arch/x86_64/atomic.c — arch_atomic_or_u64 / arch_atomic_and_u64
// strong override (spec: docs/arch/cross-boundary-symbols.md §2.3).
//
// x86_64 uses the LOCK prefix on the read-modify-write ALU instructions
// (`lock orq`, `lock andq`) for atomic RMW. The LOCK prefix makes the
// memory bus access exclusive across cores — SMP-safe.
// Memory-order semantics:
//   * `lock orq %0, (%1)` is an *acquire-release* RMW on x86: every LOCK'd
//     instruction acts as a full memory fence in practice (no load/store on
//     this CPU may be reordered past it in either direction). This is
//     stronger than the aarch64 override (acq_rel via ldaxr+stlxr) but
//     indistinguishable in the C memory model for the softirq_status
//     use case (per-CPU single-writer + per-CPU reader).
#include <arch/atomic.h>

void arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock orq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

void arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock andq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}