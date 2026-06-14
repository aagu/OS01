#ifndef __KERNEL_ARCH_CPU_H__
#define __KERNEL_ARCH_CPU_H__

#include <stdint.h>

// Maximum number of CPUs supported (compile-time array sizing).
// Must be >= the largest possible MADT LAPIC count (MAX_LAPICS).
// At runtime, num_cpus (see percpu.h) holds the actual count.
#define NR_CPUS 8

// ── Atomic operations (SMP-safe with `lock` prefix) ────────────

// Atomically add `val` to *ptr, return the OLD value.
static inline uint64_t atomic_fetch_add(volatile uint64_t *ptr, uint64_t val)
{
    __asm__ __volatile__(
        "lock xaddq %0, %1"
        : "+r"(val), "+m"(*ptr)
        :
        : "memory"
    );
    return val;
}

// Atomically subtract `val` from *ptr, return the OLD value.
static inline uint64_t atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val)
{
    return atomic_fetch_add(ptr, -(int64_t)val);
}

// Atomically increment *ptr, return the NEW value.
static inline uint64_t atomic_inc(volatile uint64_t *ptr)
{
    return atomic_fetch_add(ptr, 1) + 1;
}

// Atomically read *ptr (full barrier on x86 via LOCK prefix on the read side).
// On x86, aligned 64-bit loads are atomic, but we add a compiler barrier
// to prevent reordering.
static inline uint64_t atomic_read(volatile uint64_t *ptr)
{
    uint64_t val;
    __asm__ __volatile__("movq %1, %0" : "=r"(val) : "m"(*ptr) : "memory");
    return val;
}

// Atomically write *ptr (full barrier).
static inline void atomic_write(volatile uint64_t *ptr, uint64_t val)
{
    __asm__ __volatile__("xchgq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
}

// Atomic compare-and-swap: if *ptr == old, set *ptr = new and return 1.
static inline int atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
    uint8_t result;
    __asm__ __volatile__(
        "lock cmpxchgq %3, %1\n\t"
        "sete %0"
        : "=a"(result), "+m"(*ptr)
        : "a"(old), "r"(new)
        : "memory"
    );
    return result;
}

// Atomic test-and-set: store `val` to *ptr, return the OLD value.
static inline uint64_t atomic_xchg(volatile uint64_t *ptr, uint64_t val)
{
    __asm__ __volatile__(
        "xchgq %0, %1"
        : "+r"(val), "+m"(*ptr)
        :
        : "memory"
    );
    return val;
}

#endif
