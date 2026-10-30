#ifndef KERNEL_ARCH_X86_64_CONTEXT_H
#define KERNEL_ARCH_X86_64_CONTEXT_H

/* x86_64 thread context — include via the <arch/context.h> facade, never
 * directly (the facade defines STACK_SIZE and the task_t typedef before
 * this header is reached; that ordering is part of the contract).
 *
 * thread_t keeps the exact pre-facade field order and field offsets —
 * pinned below with _Static_assert so a reorder is a build break, not a
 * silent ABI drift (series index: "x86 thread_t field order and offsets
 * must not move").
 *
 * The switch_to inline assembly moved here verbatim from
 * kernel/include/sched/task.h (as arch_task_switch): x86 resumes by
 * returning through a pushed thread->rip, so the switch stays inline
 * assembly (spec §5.1 permits the two backends to differ at the machine
 * level; the facade contract is what must match). */

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

typedef struct thread_struct
{
    uint64_t rsp0; // in tss, points to the base of stack

    uint64_t rip;
    uint64_t rsp; // current stack pointer, points to the top of stack

    uint64_t fs;
    uint64_t gs;

    uint64_t cr3; // page table base (physical address for CR3)

    uint64_t cr2;
    uint64_t trap_nr;
    uint64_t error_code;

    // IRQ state saved by schedule() when this task entered it.
    // Per-TASK (not per-CPU): a per-CPU global gets overwritten by
    // the next task that calls schedule() on the same CPU, so the
    // resumed task would restore another task's flags (e.g. IRQ
    // opened in ret_from_intr -> nested tick frame -> #PF with
    // RFLAGS-as-RIP).  Saved in thread so each task restores its
    // own state when switched back in.
    uint64_t sched_flags;
} thread_t;

/* ── Layout pin: offsets must not drift (series constraint) ── */
_Static_assert(offsetof(thread_t, rsp0)        ==  0, "x86 thread_t rsp0 offset drift");
_Static_assert(offsetof(thread_t, rip)         ==  8, "x86 thread_t rip offset drift");
_Static_assert(offsetof(thread_t, rsp)         == 16, "x86 thread_t rsp offset drift");
_Static_assert(offsetof(thread_t, fs)          == 24, "x86 thread_t fs offset drift");
_Static_assert(offsetof(thread_t, gs)          == 32, "x86 thread_t gs offset drift");
_Static_assert(offsetof(thread_t, cr3)         == 40, "x86 thread_t cr3 offset drift");
_Static_assert(offsetof(thread_t, cr2)         == 48, "x86 thread_t cr2 offset drift");
_Static_assert(offsetof(thread_t, trap_nr)     == 56, "x86 thread_t trap_nr offset drift");
_Static_assert(offsetof(thread_t, error_code)  == 64, "x86 thread_t error_code offset drift");
_Static_assert(offsetof(thread_t, sched_flags) == 72, "x86 thread_t sched_flags offset drift");
_Static_assert(sizeof(thread_t) == 80, "x86 thread_t size drift");

/* ── Current task ──────────────────────────────────────────────
 * task_t lives at the base of its task_union, so masking RSP against
 * STACK_SIZE yields the running task — but ONLY on a task stack.  On an
 * IST exception stack use the TSS.rsp0 route instead (see
 * kernel/arch/x86_64/intr/trap.c); on the boot/AP stacks there is no
 * task at all (facade contract). */
static inline task_t *arch_current_task(void)
{
    uint64_t sp;
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
    return (task_t *)(sp & ~(STACK_SIZE - 1));
}

/* ── Raw asm text for computing `current` in inline assembly ──
 * Moved from sched/task.h.  kernel/arch/x86_64/intr/entry.S keeps its
 * own (register-parameterised) definition; this copy serves C inline
 * asm consumers. */
#define GET_CURRENT \
    "movq %rsp, %rbx \n\t" \
    "andq $-32768, %rbx \n\t"

/* ── Context switch ────────────────────────────────────────────
 * arch_task_switch(prev, next): suspends prev and transfers to next.
 * Returns only when prev is selected again, on prev's own stack and C
 * continuation (the resume label).  Machine-level shape moved verbatim
 * from sched/task.h's switch_to: push callee-saved regs, cli, save
 * prev->rsp/rip, load next->rsp, push next->rip, jmp __switch_to. */
#define arch_task_switch(prev, next) \
    do { \
        task_t *__switch_prev = (prev); \
        task_t *__switch_next = (next); \
        __asm__ __volatile__(                \
            "pushq %%rbp \n\t"       \
            "pushq %%rbx \n\t"       \
            "pushq %%r12 \n\t"       \
            "pushq %%r13 \n\t"       \
            "pushq %%r14 \n\t"       \
            "pushq %%r15 \n\t"       \
            "cli \n\t"               /* disable IRQs during stack switch */ \
            "movq %%rsp, %0 \n\t"    /* save prev->rsp */ \
            "leaq 1f(%%rip), %%rax \n\t" \
            "movq %%rax, %1 \n\t"    /* save prev->rip = resume label */ \
            "movq %4, %%rdi \n\t"    /* 1st arg: prev (SysV) */ \
            "movq %5, %%rsi \n\t"    /* 2nd arg: next (SysV) */ \
            "movq %2, %%rsp \n\t"    /* load next->rsp */ \
            "pushq %3 \n\t"          /* push next->rip */ \
            "jmp __switch_to \n\t"   \
            "1: \n\t"                \
            /* IRQ state restored by schedule() after switch */ \
            "popq %%r15 \n\t"        \
            "popq %%r14 \n\t"        \
            "popq %%r13 \n\t"        \
            "popq %%r12 \n\t"        \
            "popq %%rbx \n\t"        \
            "popq %%rbp \n\t"        \
            : "=m"(__switch_prev->thread->rsp), "=m"(__switch_prev->thread->rip) \
            : "m"(__switch_next->thread->rsp), "m"(__switch_next->thread->rip), \
              "m"(__switch_prev), "m"(__switch_next) \
            : "memory", "rax", "rcx", "rdx", "rdi", "rsi", \
              "r8", "r9", "r10", "r11", "cc" \
        ); \
    } while (0)

#endif /* __ASSEMBLER__ */
#endif /* KERNEL_ARCH_X86_64_CONTEXT_H */
