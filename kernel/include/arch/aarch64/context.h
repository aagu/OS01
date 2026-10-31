#ifndef KERNEL_ARCH_AARCH64_CONTEXT_H
#define KERNEL_ARCH_AARCH64_CONTEXT_H

/* aarch64 thread context — include via the <arch/context.h> facade,
 * never directly (the facade defines STACK_SIZE and the task_t typedef
 * before this header is reached; that ordering is part of the contract).
 *
 * Saved set per spec §5.2 (AAPCS64 call boundary): x19–x29, x30 (the
 * return continuation, the x86 "rip" equivalent) and SP; plus the
 * per-task IRQ storage sched_flags (shared semantics with x86) and the
 * stack-top/range fields the Phase 3 validation checks against
 * (usable_low <= SP < top, SP never equal to top).
 *
 * The context-switch assembly itself lands in Phase 3
 * (kernel/arch/aarch64/cpu/switch.S); this header only fixes the layout
 * and publishes the offsets that assembly #includes. */

/* ── Assembler-visible offsets (usable with and without C types) ── */
#define AARCH64_THREAD_X19_OFF     (0 * 8)
#define AARCH64_THREAD_X30_OFF     (11 * 8)
#define AARCH64_THREAD_SP_OFF      (12 * 8)
#define AARCH64_THREAD_SCHED_FLAGS_OFF (13 * 8)
#define AARCH64_THREAD_STACK_BASE_OFF  (14 * 8)
#define AARCH64_THREAD_STACK_TOP_OFF   (15 * 8)
#define AARCH64_THREAD_USABLE_LOW_OFF  (16 * 8)
#define AARCH64_THREAD_SIZE           (17 * 8)
#define AARCH64_STACK_SIZE            (32 * 1024)

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

typedef struct thread_struct {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    uint64_t x30;        /* return continuation (the "rip" equivalent) */
    uint64_t sp;         /* saved stack pointer */
    uint64_t sched_flags;/* per-task IRQ state, shared with x86 semantics */
    uint64_t stack_base; /* task_union base */
    uint64_t stack_top;  /* task_union base + STACK_SIZE */
    uint64_t usable_low; /* align_up(base + sizeof(task_t) + 256, 16) */
} thread_t;

/* ── Layout pin: Phase 3 assembly strides these exact offsets ── */
_Static_assert(offsetof(thread_t, x19)         == AARCH64_THREAD_X19_OFF,         "aarch64 thread_t x19 offset drift");
_Static_assert(offsetof(thread_t, x20)         == (1 * 8),                        "aarch64 thread_t x20 offset drift");
_Static_assert(offsetof(thread_t, x21)         == (2 * 8),                        "aarch64 thread_t x21 offset drift");
_Static_assert(offsetof(thread_t, x22)         == (3 * 8),                        "aarch64 thread_t x22 offset drift");
_Static_assert(offsetof(thread_t, x23)         == (4 * 8),                        "aarch64 thread_t x23 offset drift");
_Static_assert(offsetof(thread_t, x24)         == (5 * 8),                        "aarch64 thread_t x24 offset drift");
_Static_assert(offsetof(thread_t, x25)         == (6 * 8),                        "aarch64 thread_t x25 offset drift");
_Static_assert(offsetof(thread_t, x26)         == (7 * 8),                        "aarch64 thread_t x26 offset drift");
_Static_assert(offsetof(thread_t, x27)         == (8 * 8),                        "aarch64 thread_t x27 offset drift");
_Static_assert(offsetof(thread_t, x28)         == (9 * 8),                        "aarch64 thread_t x28 offset drift");
_Static_assert(offsetof(thread_t, x29)         == (10 * 8),                       "aarch64 thread_t x29 offset drift");
_Static_assert(offsetof(thread_t, x30)         == AARCH64_THREAD_X30_OFF,         "aarch64 thread_t x30 offset drift");
_Static_assert(offsetof(thread_t, sp)          == AARCH64_THREAD_SP_OFF,          "aarch64 thread_t sp offset drift");
_Static_assert(offsetof(thread_t, sched_flags) == AARCH64_THREAD_SCHED_FLAGS_OFF, "aarch64 thread_t sched_flags offset drift");
_Static_assert(offsetof(thread_t, stack_base)  == AARCH64_THREAD_STACK_BASE_OFF,  "aarch64 thread_t stack_base offset drift");
_Static_assert(offsetof(thread_t, stack_top)   == AARCH64_THREAD_STACK_TOP_OFF,   "aarch64 thread_t stack_top offset drift");
_Static_assert(offsetof(thread_t, usable_low)  == AARCH64_THREAD_USABLE_LOW_OFF,  "aarch64 thread_t usable_low offset drift");
_Static_assert(sizeof(thread_t) == AARCH64_THREAD_SIZE, "aarch64 thread_t size drift");
_Static_assert(STACK_SIZE == AARCH64_STACK_SIZE, "aarch64 assembly stack size drift");

/* ── Current task ──────────────────────────────────────────────
 * Same task_union stack-mask convention as x86 (spec §6): task_t sits at
 * the base of the union, so SP & ~(STACK_SIZE-1) is the running task.
 * Compiled now so Phase 2's common TUs can use `current`; valid at
 * runtime ONLY on a task stack — never on the boot stack or an AP
 * boot/work stack (no task_t there, and the mask cannot detect that).
 * Phase 3 validates and reuses this accessor after the BSP handoff. */
static inline task_t *arch_current_task(void)
{
    uint64_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    return (task_t *)(sp & ~(STACK_SIZE - 1));
}

#endif /* __ASSEMBLER__ */
#endif /* KERNEL_ARCH_AARCH64_CONTEXT_H */
