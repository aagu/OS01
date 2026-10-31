#include <sched/task.h>
#include <sched/internal.h>  // task_switch_complete — common switch tail
#include <percpu/percpu.h>
#include <arch/spinlock.h>
#include <arch/x86_64/gate.h>
#include <arch/cpu.h>
#include <arch/x86_64/tss.h>
#include <core/printk.h>
#include <log/log.h>

/* ── x86 kernel-heap window ─────────────────────────────────────
 * Direct-map region covers ALL kernel heap: kmalloc slabs are
 * allocated from alloc_pages -> phys 0..512MB mapped at
 * 0xffff800000000000..0xffff800020000000.  The old 16MB bound
 * mis-flagged every task whose stack landed above phys 16MB. */
#define X86_KHEAP_LOW  0xffff800000000000ULL
#define X86_KHEAP_HIGH 0xffff800020000000ULL

/**
 * arch_task_context_valid — validate a task's saved context before it is
 * used (facade contract; condition moved verbatim from the old inline
 * SWITCH-BAD check below).  Checks, in the same order and with the same
 * bounds as before:
 *   1. t is within the x86 kernel-heap window;
 *   2. t->thread is non-NULL and within the same window;
 *   3. t->thread->rsp lies within [t, t + STACK_SIZE] (the task's own
 *      task_union stack — a reaped task has thd_rsp pointing into poison
 *      or another task's slab-reused stack, caught here).
 * Returns true when usable; on false *reason names the violated
 * condition.  The caller logs and decides the fatal response.
 */
bool arch_task_context_valid(task_t *t, const char **reason)
{
    if ((uint64_t)t < X86_KHEAP_LOW || (uint64_t)t >= X86_KHEAP_HIGH) {
        *reason = "task pointer outside x86 kernel heap window";
        return false;
    }
    if (t->thread == NULL) {
        *reason = "thread pointer NULL";
        return false;
    }
    if ((uint64_t)t->thread < X86_KHEAP_LOW ||
        (uint64_t)t->thread >= X86_KHEAP_HIGH) {
        *reason = "thread pointer outside x86 kernel heap window";
        return false;
    }
    if ((uint64_t)t->thread->rsp < (uint64_t)t ||
        (uint64_t)t->thread->rsp > (uint64_t)t + STACK_SIZE) {
        *reason = "saved rsp outside task_union stack";
        return false;
    }
    return true;
}

/**
 * arch_task_check_prev — the two x86 PREVIOUS-context fatal checks
 * ([DIAG-5] / [DIAG-5b]), moved verbatim from schedule() in
 * kernel/sched/core.c: payload fields and the halt response are the
 * architecture's; common core calls this without reading arch fields.
 * Diagnostics use log_err (marker names and field order preserved; only
 * the serial_printk -> logger prefix changed).
 */
void arch_task_check_prev(task_t *t)
{
    // [DIAG-5] before saving prev state, verify current->thread is a
    // sane kernel-heap pointer: switch_to's asm stores
    // (movq %rsp, prev->thread->rsp / rip) would write through a
    // garbage thread pointer and corrupt memory BEFORE any crash.
    if ((uint64_t)t->thread < X86_KHEAP_LOW ||
        (uint64_t)t->thread >= X86_KHEAP_HIGH) {
        log_err("SCHED-PREV-BAD: pid=%ld thread=%p cpu=%d "
                "state=%ld on_rq=%d on_cpu=%d\n",
                t->pid, (void *)t->thread, cpu_id(),
                (long)t->state, (int)t->on_rq, (int)t->on_cpu);
        for (;;) arch_cpu_halt();
    }
    // [DIAG-5b] hardened: our SAVED thread->rsp must lie inside our
    // OWN task_union stack.  If it points elsewhere (e.g. a freed
    // 64-byte thread_t slab object), the thread struct was already
    // corrupted — and switch_to's store (movq %rsp,
    // prev->thread->rsp) would write through garbage.  Catch it
    // before the corruption spreads.
    if (t->thread->rsp < (uint64_t)t ||
        t->thread->rsp > (uint64_t)t + STACK_SIZE) {
        log_err("SCHED-PREV-BAD2: pid=%ld saved_rsp=%lx cpu=%d "
                "state=%ld cur=%p rsp0=%lx rip=%lx\n",
                t->pid, t->thread->rsp, cpu_id(),
                (long)t->state, (void *)t,
                t->thread->rsp0, t->thread->rip);
        for (;;) arch_cpu_halt();
    }
}

/**
 * arch_dump_task_context — the per-task architecture part of the
 * hang/watchdog dump, moved verbatim from hang_dump_all()
 * (kernel/core/panic.c).  Field order and format are unchanged; raw
 * serial_printk stays because this IS the whitelisted panic/scheduler
 * fatal dump (log policy, docs/log/log.md).
 */
void arch_dump_task_context(task_t *t)
{
    if (t->thread) {
        serial_printk("    rip=%#lx rsp=%#lx cr3=%#lx\n",
                      (unsigned long)t->thread->rip,
                      (unsigned long)t->thread->rsp,
                      (unsigned long)t->thread->cr3);
    }
}

/**
 * arch_task_prepare_first_switch — arm a never-switched idle task's
 * first continuation.  schedule() calls this ONLY for next == rq->idle.
 * x86 writes the common C idle continuation (idle_task_resume) and only
 * while rip is still 0; it must NOT substitute the different assembly
 * idle_resume in arch/x86_64/intr/entry.S.
 */
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void))
{
    if (t->thread->rip == 0)
        t->thread->rip = (uint64_t)idle_entry;
}

/**
 * __switch_to — architecture-specific context switch
 *
 * Called by the switch_to() macro (in <arch/x86_64/context.h>) via
 * inline asm jmp.
 * Performs:
 *   1. Update per-CPU TSS rsp0 for ring-0 stack on next interrupt.
 *   2. Save/restore FS base selector.
 *   3. Switch page table (CR3) if needed.
 *   4. Save/restore FPU/SSE state for user tasks.
 */
void __switch_to(task_t *prev, task_t *next)
{
    // ── DEBUG VALIDATION: catch the UAF victim before jumping ──
    // next must be a kernel-heap task pointer, next->thread valid,
    // next->thread->rsp/rip inside the kernel image or heap (the exact
    // condition lives in arch_task_context_valid).  Log format is
    // unchanged from the pre-facade inline check.
    const char *switch_bad_reason = NULL;
    if (!arch_task_context_valid(next, &switch_bad_reason)) {
        log_err("SWITCH-BAD: next=%p pid=%ld state=%ld cpu=%d "
                "on_rq=%d on_cpu=%d thread=%p\n",
                (void *)next, next ? next->pid : -1,
                next ? next->state : -1,
                cpu_id(),
                next ? (int)next->on_rq : -1,
                next ? (int)next->on_cpu : -1,
                (void *)(next ? next->thread : NULL));
        log_err("SWITCH-BAD: prev=%p pid=%ld thread=%p\n",
                (void *)prev, prev ? prev->pid : -1,
                (void *)(prev ? prev->thread : NULL));
        for (;;) arch_cpu_halt();  // halt instead of crashing
    }

    percpu_t *cpu = this_cpu();
    cpu->tss->rsp0 = next->thread->rsp0;

    set_tss64(cpu->tss_hw,
              cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
              cpu->tss->ist1, cpu->tss->ist2, cpu->tss->ist3,
              cpu->tss->ist4, cpu->tss->ist5, cpu->tss->ist6,
              cpu->tss->ist7);

    // Save/restore FS selector (used by kernel threads).
    // GS base is per-CPU and set ONCE via MSR — never
    // touch it here (loading a non-null GS selector would
    // reload the base from the GDT, clobbering the MSR).
    __asm__ __volatile__("movq %%fs, %0 \n\t":"=a"(prev->thread->fs));
    __asm__ __volatile__("movq %0, %%fs \n\t"::"a"(next->thread->fs));

    // Switch page table if the next task has its own address space
    if (next->thread->cr3 && next->thread->cr3 != prev->thread->cr3) {
        __asm__ __volatile__("movq %0, %%cr3" :: "r"(next->thread->cr3) : "memory");
    }

    // Save/restore FPU/SSE state.  The kernel never uses FPU
    // (-mno-sse -mno-80387), but user programs may.  clts ensures
    // CR0.TS=0 so fxsave/fxrstor don't #NM.
    // fpu_save is a raw malloc ptr; align to 16 bytes for FXSAVE.
    if (prev->fpu_save) {
        uint64_t area = ((uint64_t)prev->fpu_save + 15) & ~15ULL;
        __asm__ __volatile__(
            "clts                \n\t"
            "fxsave64 (%0)       \n\t"
            :: "r"(area) : "memory"
        );
    }
    if (next->fpu_save) {
        uint64_t area = ((uint64_t)next->fpu_save + 15) & ~15ULL;
        __asm__ __volatile__(
            "clts                \n\t"
            "fxrstor64 (%0)      \n\t"
            :: "r"(area) : "memory"
        );
    }

    /* Common switch tail: task_finish_switch(prev) publishes on_cpu=0
     * and, for a PF_SELF_REAP kthread, reclaims prev's slab objects.
     * The tail captures everything it needs from prev BEFORE the
     * publish — after on_cpu clears, a waiter may free prev — and never
     * reads prev afterwards. We already run on NEXT's stack (switch_to
     * switched rsp), so freeing prev's stack is safe. Self-switch
     * (actual_prev == current) is a no-op inside the tail. */
    task_switch_complete(prev);
}
