#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/cpu.h>

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
    percpu_t *cpu = this_cpu();
    cpu->tss->rsp0 = next->thread->rsp0;

    set_tss64(cpu->tss->rsp0, cpu->tss->rsp1, cpu->tss->rsp2,
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
}
