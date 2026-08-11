#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/cpu.h>
#include <kernel/printk.h>

/**
 * __switch_to — architecture-specific context switch
 *
 * Called by the switch_to() macro (in task.h) via inline asm jmp.
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
    // next->thread->rsp/rip inside the kernel image or heap.
    // Direct-map region covers ALL kernel heap: kmalloc slabs are
    // allocated from alloc_pages -> phys 0..512MB mapped at
    // 0xffff800000000000..0xffff800020000000.  The old 16MB bound
    // mis-flagged every task whose stack landed above phys 16MB.
    if ((uint64_t)next < 0xffff800000000000ULL ||
        (uint64_t)next >= 0xffff800020000000ULL ||
        next->thread == NULL ||
        (uint64_t)next->thread < 0xffff800000000000ULL ||
        (uint64_t)next->thread >= 0xffff800020000000ULL ||
        // Real UAF check: the task's saved kernel-stack pointer must
        // live inside its own task_union stack.  A reaped task has
        // thd_rsp pointing into poison (0xffffffffffffffff) or another
        // task's stack (slab reuse) -> caught here.
        (uint64_t)next->thread->rsp < (uint64_t)next ||
        (uint64_t)next->thread->rsp > (uint64_t)next + STACK_SIZE) {
        serial_printk("SWITCH-BAD: next=%p pid=%ld state=%ld cpu=%d "
                      "on_rq=%d on_cpu=%d thread=%p\n",
                      (void *)next, next ? next->pid : -1,
                      next ? next->state : -1,
                      cpu_id(),
                      next ? (int)next->on_rq : -1,
                      next ? (int)next->on_cpu : -1,
                      (void *)(next ? next->thread : NULL));
        serial_printk("SWITCH-BAD: prev=%p pid=%ld thread=%p\n",
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

    // prev has now fully left the CPU: its kernel stack is no
    // longer in use.  Clear on_cpu so the zombie reaper may free it
    // (a task that set TASK_ZOMBIE and ran its final schedule()).
    // RELEASE store: paired with task_wake/reaper ACQUIRE loads.
    __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
}
