#ifndef _ARCH_PERCPU_H
#define _ARCH_PERCPU_H

#include <stdint.h>

// Returns the raw per-CPU data pointer for the current CPU.
// Caller (percpu.h's this_cpu()) casts to percpu_t *.
// Returns void * to avoid circular dependency with percpu_t definition.
#ifdef __x86_64__
static inline void *arch_this_cpu_ptr(void) {
    void *ptr;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(ptr));
    return ptr;
}
#elif defined(__aarch64__)
static inline void *arch_this_cpu_ptr(void) {
    void *ptr;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(ptr));
    return ptr;
}
#else
#error "Unsupported architecture"
#endif

#endif
