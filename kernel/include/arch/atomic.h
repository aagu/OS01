#ifndef _ARCH_ATOMIC_H
#define _ARCH_ATOMIC_H

#include <stdint.h>

#ifdef __x86_64__

static inline uint64_t arch_atomic_fetch_add(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("lock xaddq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
    return val;
}

static inline uint64_t arch_atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val) {
    return arch_atomic_fetch_add(ptr, -(int64_t)val);
}

static inline uint64_t arch_atomic_inc(volatile uint64_t *ptr) {
    return arch_atomic_fetch_add(ptr, 1) + 1;
}

static inline uint64_t arch_atomic_read(volatile uint64_t *ptr) {
    uint64_t val;
    __asm__ __volatile__("movq %1, %0" : "=r"(val) : "m"(*ptr) : "memory");
    return val;
}

static inline void arch_atomic_write(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("xchgq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
}

static inline int arch_atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new) {
    uint8_t result;
    __asm__ __volatile__("lock cmpxchgq %3, %1; sete %0"
                         : "=a"(result), "+m"(*ptr) : "a"(old), "r"(new) : "memory");
    return result;
}

static inline uint64_t arch_atomic_xchg(volatile uint64_t *ptr, uint64_t val) {
    __asm__ __volatile__("xchgq %0, %1" : "+r"(val), "+m"(*ptr) : : "memory");
    return val;
}

#elif defined(__aarch64__)

static inline uint64_t arch_atomic_fetch_add(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t old, tmp;
    __asm__ __volatile__(
        "1: ldxr %0, [%2]       \n\t"
        "   add  %1, %0, %3     \n\t"
        "   stxr %w4, %1, [%2]  \n\t"
        "   cbnz %w4, 1b        \n\t"
        : "=&r"(old), "=&r"(tmp), "+r"(ptr)
        : "r"(val), "r"(0)
        : "memory"
    );
    return old;
}

static inline uint64_t arch_atomic_fetch_sub(volatile uint64_t *ptr, uint64_t val)
{
    return arch_atomic_fetch_add(ptr, -(int64_t)val);
}

static inline uint64_t arch_atomic_inc(volatile uint64_t *ptr)
{
    return arch_atomic_fetch_add(ptr, 1) + 1;
}

static inline uint64_t arch_atomic_read(volatile uint64_t *ptr)
{
    uint64_t val;
    __asm__ __volatile__("ldr %0, [%1]" : "=r"(val) : "r"(ptr) : "memory");
    return val;
}

static inline void arch_atomic_write(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t tmp;
    /* LDAXR + STLXR pair: the STLXR is a RELEASE store.  This gives
     * spin_unlock() the release semantics that pairs with the ACQUIRE
     * in the matching spin_lock's LDAXR/CAS. */
    __asm__ __volatile__(
        "1: ldaxr %0, [%1]       \n\t"
        "   stlxr %w0, %2, [%1]  \n\t"
        "   cbnz %w0, 1b        \n\t"
        : "=&r"(tmp) : "r"(ptr), "r"(val) : "memory"
    );
}

static inline int arch_atomic_cas(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
    uint64_t cur;
    int result;
    /* LDAXR = ACQUIRE load: a successful CAS here is an ACQUIRE that
     * orders subsequent loads/stores after the critical section's
     * release in the unlock path.  STLXR is RELEASE on the store side. */
    __asm__ __volatile__(
        "1: ldaxr %0, [%2]       \n\t"
        "   cmp  %0, %3         \n\t"
        "   b.ne 2f             \n\t"
        "   stlxr %w1, %4, [%2]  \n\t"
        "   cbnz %w1, 1b        \n\t"
        "   mov  %w1, #1         \n\t"
        "   b    3f             \n\t"
        "2: mov  %w1, #0         \n\t"
        "3:                     \n\t"
        : "=&r"(cur), "=&r"(result) : "r"(ptr), "r"(old), "r"(new) : "memory"
    );
    return result;
}

static inline uint64_t arch_atomic_xchg(volatile uint64_t *ptr, uint64_t val)
{
    uint64_t old;
    int tmp;
    /* LDAXR + STLXR: ACQUIRE on the read, RELEASE on the write. */
    __asm__ __volatile__(
        "1: ldaxr %0, [%2]       \n\t"
        "   stlxr %w1, %3, [%2]  \n\t"
        "   cbnz %w1, 1b        \n\t"
        : "=&r"(old), "=&r"(tmp) : "r"(ptr), "r"(val) : "memory"
    );
    return old;
}

#else
#error "Unsupported architecture"
#endif

#endif
