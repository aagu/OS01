#ifndef __KERNEL_ARCH_CPU_H__
#define __KERNEL_ARCH_CPU_H__

#include <stdint.h>

// Maximum number of CPUs supported at compile time.
//
// Determines the size of percpu_data[], init_tss[], init_task[], and the
// MADT LAPIC/x2APIC entry buffer.  If firmware reports more enabled
// processors than this limit, the extras are silently ignored.
//
// There is no hard x86 limit at 8 — APIC IDs span 8 bits (xAPIC) or 32
// bits (x2APIC).  This value is a design choice for a hobby kernel:
// enough to exercise SMP correctness without wasting static memory on
// arrays that will never be filled under QEMU (default -smp 4).
//
// Bump to 16, 64, or 256 as needed.  Memory cost per slot:
//   percpu_t    ~56 B   (one per CPU)
//   tss_struct  104 B   (one per CPU)
//   task_t *      8 B   (init_task[])
// Total: ~168 B per extra slot — negligible.
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

// ── Time-Stamp Counter (TSC) ────────────────────────────

// Read the full 64-bit TSC via RDTSC.
// Returns the current TSC value.  On invariant-TSC CPUs
// (CPUID 0x80000007 EDX bit 8), the counter ticks at a
// constant rate irrespective of P-states.
static inline uint64_t rdtsc(void)
{
    uint32_t low, high;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// Serialising read — executes CPUID first to flush the
// instruction pipeline, then RDTSC.  Use this when ordering
// relative to surrounding code matters.
static inline uint64_t rdtscp_serialized(void)
{
    uint32_t low, high;
    __asm__ __volatile__(
        "cpuid          \n\t"
        "rdtsc          \n\t"
        : "=a"(low), "=d"(high)
        : "a"(0)
        : "rbx", "rcx", "memory"
    );
    return ((uint64_t)high << 32) | low;
}

#endif
