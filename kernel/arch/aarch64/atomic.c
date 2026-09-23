// kernel/arch/aarch64/atomic.c — arch_atomic_or_u64 / arch_atomic_and_u64
// strong override (spec: docs/arch/cross-boundary-symbols.md §2.3).
//
// Implementation: LR/SC retry loop. AArch64 Large System Extensions (LSE)
// provide single-instruction ldset/stclr with release/acquire semantics;
// when LSE is unavailable (HWCAP_ATOMIC absent) the fallback below works
// on any ARMv8.0+ CPU. We pick the LR/SC fallback so the kernel runs on
// the broadest set of aarch64 cores.
//
// Memory-order semantics:
//   ldaxr — load-acquire exclusive. Loads *addr with acquire semantics (no
//           subsequent load/store in this CPU can be reordered BEFORE this
//           load) and marks the address as exclusive monitor for this CPU.
//   stlxr — store-release exclusive. Stores new_val to *addr if exclusive
//           monitor still belongs to this CPU; status=0 on success, status=1
//           on failure (and *addr is NOT modified on failure). Release
//           semantics for the store (no prior load/store can be reordered
//           AFTER it).
//   "memory" clobber — tells compiler not to reorder loads/stores around the
//           asm. The acquire-on-load + release-on-store pair is an
//           **acquire-release RMW**, NOT seq_cst (no dmb ish between
//           independent RMWs on the same CPU). Sufficient for the
//           softirq_status use case (per-CPU single-writer + per-CPU
//           reader).
//
// Register-allocation constraint (R3):
//   stlxr's `%w[status]` operand writes the *low 32 bits* of its register
//   as the status word. If [status] shared the same register as [new_val],
//   the store would clobber the value being stored. Three independent
//   `=&r` early-clobber constraints force separate registers for [old],
//   [new_val], and [status].
#include <arch/atomic.h>

void arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   orr    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

void arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   and    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}