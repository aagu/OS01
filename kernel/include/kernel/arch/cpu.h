#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/cpu.h>    // for rdtsc(), NR_CPUS
#include <kernel/arch/x86_64/regs.h>   // IA32_EFER, EFER_NXE, IA32_GS_BASE

static inline void     arch_cpu_halt(void)      { hlt(); }
static inline void     arch_cpu_pause(void)     { __asm__ __volatile__("pause"); }
static inline void     arch_nop(void)           { __asm__ __volatile__("nop"); }
static inline uint64_t arch_cycle_counter(void) { return rdtsc(); }

// TSC 频率（Hz）。实现见 kernel/arch/x86_64/time.c（CPUID 15h → RTC PIE → 0）。
uint64_t arch_cycle_freq(void);

// 启动 arch tick 源（x86: LAPIC 周期模式）。实现见 kernel/arch/x86_64/time.c。
// 返回是否启动成功；失败由 clockevent 层回退 PIT。
bool arch_tick_start(void);

// NR_CPUS — architecture-specific max CPU count.
// Defined here (as well as in arch/x86_64/cpu.h for backward compat)
// so generic headers like task.h and percpu.h can find it.
#ifndef NR_CPUS
#define NR_CPUS 8
#endif

// Enable No-eXecute: set EFER.NXE (bit 11)
static inline void arch_cpu_enable_nx(void) {
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(IA32_EFER));
    if (!(eax & EFER_NXE)) {
        eax |= EFER_NXE;
        __asm__ __volatile__("wrmsr" : : "a"(eax), "d"(edx), "c"(IA32_EFER));
    }
}

// Set per-CPU data base pointer (GS on x86, tpidr_el1 on aarch64)
static inline void arch_set_percpu_base(void *ptr) {
    uint32_t lo = (uint32_t)(uintptr_t)ptr;
    uint32_t hi = (uint32_t)((uintptr_t)ptr >> 32);
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(IA32_GS_BASE) : "memory");
}

#elif defined(__aarch64__)

#include <stdint.h>

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

static inline void arch_cpu_halt(void)
{
    __asm__ __volatile__("wfi" ::: "memory");
}

static inline void arch_cpu_pause(void)
{
    __asm__ __volatile__("yield" ::: "memory");
}

static inline void arch_nop(void)
{
    __asm__ __volatile__("nop");
}

static inline uint64_t arch_cycle_counter(void)
{
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t arch_cycle_freq(void)
{
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

// Enable No-eXecute: set SCTLR_EL1.WXN (bit 19)
static inline void arch_cpu_enable_nx(void)
{
    uint64_t sctlr;
    __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 19);
    __asm__ __volatile__("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
}

// Set per-CPU data base pointer (TPIDR_EL1 on aarch64)
static inline void arch_set_percpu_base(void *ptr)
{
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"((uint64_t)ptr) : "memory");
}

#else
#error "Unsupported architecture"
#endif

#endif
