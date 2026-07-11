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
#error "aarch64 atomic.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
