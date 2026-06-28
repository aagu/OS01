/* test/mock/test_platform.h — Minimal host platform stub */
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H

#ifndef __is_kernel
#  define __is_kernel 1
#endif
#ifndef __is_libk
#  define __is_libk 1
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strstr from host libc (not in our custom string.h) */
extern char *strstr(const char *haystack, const char *needle);

#ifndef do_div
#define do_div(n,base) ({ unsigned long __rem__ = (n) % (base); (n) /= (base); __rem__; })
#endif
#ifndef is_digit
#define is_digit(c) ((c) >= '0' && (c) <= '9')
#endif
#define ZEROPAD 1
#define SIGN 2
#define PLUS 4
#define SPACE 8
#define LEFT 16
#define SPECIAL 32
#define SMALL 64

#define cli()
#define sti()

#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

#define Phy_To_Virt(x) ((void*)(uintptr_t)(x))
#define Virt_To_Phy(x) ((uintptr_t)(x))

#define PAGE_2M_SHIFT 21
#define PAGE_4K_SHIFT 12
#define PAGE_2M_SIZE (1UL << 21)
#define PAGE_4K_SIZE (1UL << 12)
#define PAGE_4K_MASK (~(PAGE_4K_SIZE - 1))
#define PAGE_2M_MASK (~(PAGE_2M_SIZE - 1))
#define PAGE_GDT_SHIFT 39
#define PAGE_1G_SHIFT 30

#define NR_CPUS 8

/* percpu stub */
typedef struct {
    uint64_t self, need_resched;
    uint32_t cpu_id, apic_id, online, scheduler_ok;
    void *tss;
    uint32_t tlb_wanted, tlb_ack;
    void *run_queue, *idle;
    uint64_t schedule_count, tsc_boot;
} percpu_t;
static inline percpu_t *this_cpu(void) { return NULL; }
static inline uint32_t cpu_id(void) { return 0; }
extern uint32_t num_cpus;

/* Spinlock stub */
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)l; (void)f; }

/* I/O port stubs */
static inline uint8_t inb(uint16_t p) { (void)p; return 0; }
static inline void outb(uint16_t p, uint8_t d) { (void)p; (void)d; }

/* MSR stubs */
static inline uint64_t rdmsr(uint32_t m) { (void)m; return 0; }
static inline void wrmsr(uint32_t m, uint64_t v) { (void)m; (void)v; }

/* TSC stub */
static inline uint64_t rdtsc(void) { return 0; }
static inline uint64_t *get_cr3(void) { return NULL; }

/* Arch linkage */
#define asmlinkage
#define L1_CACHE_BYTES 32

#endif /* TEST_PLATFORM_H */
