#ifndef _ARCH_BARRIER_H
#define _ARCH_BARRIER_H

#ifdef __x86_64__
#define arch_mb()  __asm__ __volatile__("mfence" ::: "memory")
#define arch_rmb() __asm__ __volatile__("lfence" ::: "memory")
#define arch_wmb() __asm__ __volatile__("sfence" ::: "memory")
#elif defined(__aarch64__)
#define arch_mb()  __asm__ __volatile__("dmb sy" ::: "memory")
#define arch_rmb() __asm__ __volatile__("dmb ld" ::: "memory")
#define arch_wmb() __asm__ __volatile__("dmb st" ::: "memory")
#else
#error "Unsupported architecture"
#endif

#endif
