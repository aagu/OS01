#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <stdint.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/cpu.h>    // for rdtsc(), NR_CPUS

static inline void     arch_cpu_halt(void)      { hlt(); }
static inline void     arch_cpu_pause(void)     { __asm__ __volatile__("pause"); }
static inline void     arch_nop(void)           { __asm__ __volatile__("nop"); }
static inline uint64_t arch_cycle_counter(void) { return rdtsc(); }

// NR_CPUS — architecture-specific max CPU count.
// Defined here (as well as in arch/x86_64/cpu.h for backward compat)
// so generic headers like task.h and percpu.h can find it.
#ifndef NR_CPUS
#define NR_CPUS 8
#endif

// Enable No-eXecute: set EFER.NXE (bit 11)
static inline void arch_cpu_enable_nx(void) {
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
    if (!(eax & (1 << 11))) {
        eax |= (1 << 11);
        __asm__ __volatile__("wrmsr" : : "a"(eax), "d"(edx), "c"(0xC0000080));
    }
}

// Set per-CPU data base pointer (GS on x86, tpidr_el1 on aarch64)
static inline void arch_set_percpu_base(void *ptr) {
    uint32_t lo = (uint32_t)(uintptr_t)ptr;
    uint32_t hi = (uint32_t)((uintptr_t)ptr >> 32);
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000101) : "memory");
}

#elif defined(__aarch64__)
#error "aarch64 cpu.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
