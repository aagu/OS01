#ifndef _ARCH_CACHE_H
#define _ARCH_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __x86_64__

static inline void arch_flush_dcache(void *addr, size_t len) { (void)addr; (void)len; }
static inline void arch_inval_dcache(void *addr, size_t len) { (void)addr; (void)len; }

#elif defined(__aarch64__)

// Clean data cache by virtual address to point of coherency
static inline void arch_flush_dcache(void *va, size_t size)
{
    uintptr_t addr = (uintptr_t)va;
    uintptr_t end = addr + size;
    for (; addr < end; addr += 64) {
        __asm__ __volatile__("dc cvac, %0" :: "r"(addr) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}

// Invalidate data cache by virtual address to point of coherency
static inline void arch_inval_dcache(void *va, size_t size)
{
    uintptr_t addr = (uintptr_t)va;
    uintptr_t end = addr + size;
    for (; addr < end; addr += 64) {
        __asm__ __volatile__("dc ivac, %0" :: "r"(addr) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
}

#else
#error "Unknown architecture"
#endif

#endif
