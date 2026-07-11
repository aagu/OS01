#ifndef _ARCH_IO_H
#define _ARCH_IO_H

#include <stdint.h>

#ifdef __x86_64__
#include <kernel/arch/x86_64/hw.h>

// MMIO
static inline uint8_t  arch_readb(volatile void *a) { return *(volatile uint8_t  *)a; }
static inline uint16_t arch_readw(volatile void *a) { return *(volatile uint16_t *)a; }
static inline uint32_t arch_readl(volatile void *a) { return *(volatile uint32_t *)a; }
static inline uint64_t arch_readq(volatile void *a) { return *(volatile uint64_t *)a; }
static inline void     arch_writeb(volatile void *a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void     arch_writew(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void     arch_writel(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void     arch_writeq(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }

// Port I/O — alias existing hw.h functions
#define arch_inb   inb
#define arch_outb  outb
#define arch_inw   inw
#define arch_outw  outw
#define arch_ind   ind
#define arch_outd  outd

#elif defined(__aarch64__)
static inline uint8_t  arch_readb(volatile void *a) { return *(volatile uint8_t  *)a; }
static inline uint16_t arch_readw(volatile void *a) { return *(volatile uint16_t *)a; }
static inline uint32_t arch_readl(volatile void *a) { return *(volatile uint32_t *)a; }
static inline uint64_t arch_readq(volatile void *a) { return *(volatile uint64_t *)a; }
static inline void     arch_writeb(volatile void *a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
static inline void     arch_writew(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void     arch_writel(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void     arch_writeq(volatile void *a, uint64_t v) { *(volatile uint64_t *)a = v; }
static inline uint8_t  arch_inb(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outb(uint16_t p, uint8_t d) { (void)p; (void)d; __builtin_trap(); }
static inline uint16_t arch_inw(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outw(uint16_t p, uint16_t d) { (void)p; (void)d; __builtin_trap(); }
static inline uint32_t arch_ind(uint16_t p)  { (void)p; __builtin_trap(); return 0; }
static inline void     arch_outd(uint16_t p, uint32_t d) { (void)p; (void)d; __builtin_trap(); }
#else
#error "Unsupported architecture"
#endif

#endif
