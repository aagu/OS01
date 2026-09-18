/* hosttests/mock/clocksource_test_runtime.h
 *
 * Host-only runtime surface for compiling kernel/time/clocksource.c
 * against a host clang toolchain (no aarch64 inline asm, no full
 * kernel headers).
 *
 * Pattern mirrors hosttests/mock/poll_test_runtime.h: short-circuit the
 * kernel headers with `#define _XXX_H` BEFORE they are included, then
 * provide host-friendly stubs for the symbols clocksource.c and the
 * inline clocksource_read_ns in <time/clocksource.h> need.
 *
 * IMPORTANT: this header MUST be -included BEFORE any kernel header
 * (and BEFORE test_platform.h — the clocksource test rule in the
 * Makefile does NOT use $(FRAMEWORK_INC)). The Makefile rule uses
 * `-include clocksource_test_runtime.h` and skips the -include of
 * test_platform.h, so this header owns the entire host stub surface.
 *
 * Symbols supplied here (defined in clocksource_test_stubs.c):
 *   - jiffies                        (host-side uint64)
 *   - percpu_data[0]                 (one per-CPU stub with tsc_offset=0)
 *   - host_mock_cycle_freq           (writable from tests; arch_cycle_freq returns it)
 *   - host_mock_cycle_counter        (writable; arch_cycle_counter returns it)
 *   - this_cpu() / cpu_id()          (return percpu_data[0])
 */
#ifndef OS01_CLOCKSOURCE_TEST_RUNTIME_H
#define OS01_CLOCKSOURCE_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>

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
#define PAGE_PGD_SHIFT 39
#define PAGE_1G_SHIFT 30

#define NR_CPUS 8

/* Short-circuit heavy kernel headers so clocksource.c pulls host stubs. */
#define _ARCH_CPU_H
#define _ARCH_SPINLOCK_H
#define _ARCH_PERCPU_H
#define _KERNEL_PERCPU_H
#define KERNEL_TASK_H

/* jiffies — declared extern in <time/timer.h>. The clocksource inactive
 * fallback returns jiffies * 10000000ULL. */
extern volatile uint64_t jiffies;

/* Minimal percpu: only `tsc_offset` is read by clocksource_read_ns
 * (inline in <time/clocksource.h>). Layout intentionally tiny — NOT the
 * real kernel percpu_t; production <percpu/percpu.h> is short-circuited
 * above so this typedef takes its place. Name MUST be `percpu_t` so the
 * `this_cpu()->tsc_offset` reference in clocksource_read_ns() compiles. */
typedef struct percpu {
    uint64_t self;
    uint64_t need_resched;
    int64_t  tsc_offset;        /* bsp_tsc - ap_tsc; host == 0 */
} percpu_t;

extern percpu_t percpu_data[1];

/* this_cpu / cpu_id — host stubs that always return CPU 0. */
static inline percpu_t *this_cpu(void) { return &percpu_data[0]; }
static inline uint32_t cpu_id(void) { return 0; }

typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)l; (void)f; }

/* I/O port stubs (unused by clocksource.c but referenced by transitive
 * kernel headers that we short-circuit). */
static inline uint8_t inb(uint16_t p) { (void)p; return 0; }
static inline void outb(uint16_t p, uint8_t d) { (void)p; (void)d; }

/* MSR stubs (unused, but referenced via <arch/x86_64/regs.h> in some
 * host-transitive includes). */
static inline uint64_t rdmsr(uint32_t m) { (void)m; return 0; }
static inline void wrmsr(uint32_t m, uint64_t v) { (void)m; (void)v; }

/* x86 TSC stub. */
static inline uint64_t rdtsc(void) { return 0; }
static inline uint64_t *get_cr3(void) { return NULL; }

#define asmlinkage
#define L1_CACHE_BYTES 32

/* Mock-controlled cycle counter / frequency. The test file writes
 * these before exercising clocksource_init / clocksource_read_ns. */
extern uint64_t host_mock_cycle_freq;
extern uint64_t host_mock_cycle_counter;

/* Host stubs for the arch inline-asm symbols clocksource.c uses. They
 * intentionally are NOT static inline here — the production header
 * was short-circuited (see #define _ARCH_CPU_H above), so they
 * resolve to these extern symbols at link time. */
uint64_t arch_cycle_freq(void);
uint64_t arch_cycle_counter(void);

#endif /* OS01_CLOCKSOURCE_TEST_RUNTIME_H */
