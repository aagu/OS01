#include <arch/unwind.h>
#include <arch/x86_64/unwind.h>
#include <arch/x86_64/gate.h>
#include <arch/x86_64/tss.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <stddef.h>

extern const char x86_unwind_load_next_fp[];
extern const char x86_unwind_load_ret_pc[];
extern const char x86_unwind_load_fail[];

static inline bool x86_is_canonical_va(uintptr_t addr)
{
    int64_t top = (int64_t)addr >> 47;
    return top == 0 || top == -1;
}

static inline bool x86_is_high_half_canonical(uintptr_t addr)
{
    return ((int64_t)addr >> 47) == -1;
}

static inline uintptr_t x86_read_gs_base(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000101U));
    return ((uintptr_t)hi << 32) | (uintptr_t)lo;
}

__attribute__((no_stack_protector))
bool x86_unwind_fixup(struct pt_regs *regs, uint64_t error, uintptr_t cr2)
{
    if (!regs) {
        return false;
    }
    if ((regs->cs & 3u) != 0) {
        return false;
    }
    if ((error & ~UINT64_C(1)) != 0) {
        return false;
    }

    uintptr_t rip = (uintptr_t)regs->rip;
    uintptr_t base = (uintptr_t)regs->rdi;
    if (rip == (uintptr_t)x86_unwind_load_next_fp) {
        if (cr2 != base) {
            return false;
        }
    } else if (rip == (uintptr_t)x86_unwind_load_ret_pc) {
        if (base > UINTPTR_MAX - 8u || cr2 != base + 8u) {
            return false;
        }
    } else {
        return false;
    }

    regs->rip = (uint64_t)(uintptr_t)x86_unwind_load_fail;
    return true;
}

__attribute__((no_stack_protector))
bool arch_unwind_read_frame(uintptr_t fp, struct unwind_frame *out)
{
    if (!out) {
        return false;
    }
    if ((fp & (sizeof(uintptr_t) - 1u)) != 0) {
        return false;
    }
    if (fp > UINTPTR_MAX - sizeof(struct unwind_frame)) {
        return false;
    }
    if (!x86_is_canonical_va(fp) || !x86_is_canonical_va(fp + 8u)) {
        return false;
    }
    return x86_unwind_load_pair(fp, out);
}

static inline bool x86_valid_task_rsp0(uint64_t rsp0)
{
    if (!x86_is_high_half_canonical((uintptr_t)rsp0)) {
        return false;
    }
    if ((rsp0 & (STACK_SIZE - 1u)) != 0) {
        return false;
    }
    if (rsp0 < (UINT64_C(0xffff800000000000) + STACK_SIZE)) {
        return false;
    }
    return true;
}

__attribute__((no_stack_protector))
bool x86_unwind_resolve_stack(uintptr_t gs_base, uintptr_t sp,
                              uintptr_t *low_out, uintptr_t *high_out)
{
    if (low_out) {
        *low_out = 0;
    }
    if (high_out) {
        *high_out = 0;
    }

    uintptr_t pcpu_base = (uintptr_t)&percpu_data[0];
    uintptr_t pcpu_end = (uintptr_t)&percpu_data[NR_CPUS];
    if (gs_base < pcpu_base || gs_base >= pcpu_end) {
        return false;
    }
    uintptr_t delta = gs_base - pcpu_base;
    if ((delta % sizeof(percpu_t)) != 0) {
        return false;
    }
    unsigned cpu = (unsigned)(delta / sizeof(percpu_t));
    if (cpu >= NR_CPUS) {
        return false;
    }

    const percpu_t *slot = &percpu_data[cpu];
    uint32_t online = __atomic_load_n(&slot->online, __ATOMIC_ACQUIRE);

    uintptr_t cand_low = 0;
    uintptr_t cand_high = 0;

    if (online == 0) {
        /* Unpublished phase: only provable BSP slot may use static init_task_union. */
        if (cpu != 0) {
            return false;
        }
        uintptr_t base = (uintptr_t)&init_task_union;
        cand_low = (base + sizeof(task_t) + 7u) & ~(uintptr_t)7u;
        cand_high = base + STACK_SIZE;
    } else {
        if (slot->cpu_id != cpu) {
            return false;
        }
        const void *expected_tss = (cpu == 0) ? (const void *)TSS64_Table
                                              : (const void *)&init_tss[cpu];
        if (slot->tss_hw == NULL || slot->tss_hw != expected_tss) {
            return false;
        }
        uint64_t rsp0 = ((const struct tss_struct *)slot->tss_hw)->rsp0;
        if (x86_valid_task_rsp0(rsp0)) {
            uintptr_t base = (uintptr_t)(rsp0 - STACK_SIZE);
            cand_low = (base + sizeof(task_t) + 7u) & ~(uintptr_t)7u;
            cand_high = (uintptr_t)rsp0;
        } else if (cpu == 0) {
            /* BSP between percpu online and arch_task_init_platform() still
             * has boot rsp0=0x7c00 in TSS64_Table while running on init_task_union. */
            uintptr_t base = (uintptr_t)&init_task_union;
            cand_low = (base + sizeof(task_t) + 7u) & ~(uintptr_t)7u;
            cand_high = base + STACK_SIZE;
        } else {
            return false;
        }
    }

    if (cand_low >= cand_high) {
        return false;
    }
    if (sp < cand_low || sp >= cand_high) {
        return false;
    }

    if (low_out) {
        *low_out = cand_low;
    }
    if (high_out) {
        *high_out = cand_high;
    }
    return true;
}

__attribute__((no_stack_protector))
void x86_unwind_init_with_gs(struct unwind_cursor *cursor,
                             const struct pt_regs *regs,
                             bool allow_walk,
                             uintptr_t gs_base)
{
    if (!cursor || !regs) {
        return;
    }

    cursor->pc = (uintptr_t)regs->rip;
    cursor->sp = (uintptr_t)regs->rsp;
    cursor->fp = (uintptr_t)regs->rbp;
    cursor->stack_low = 0;
    cursor->stack_high = 0;
    cursor->frame_align = sizeof(uintptr_t);
    cursor->kernel_origin = ((regs->cs & 3u) == 0);

    if (!allow_walk || !cursor->kernel_origin) {
        return;
    }

    uintptr_t low = 0;
    uintptr_t high = 0;
    if (x86_unwind_resolve_stack(gs_base, cursor->sp, &low, &high)) {
        cursor->stack_low = low;
        cursor->stack_high = high;
    }
}

__attribute__((no_stack_protector))
void arch_unwind_init(struct unwind_cursor *cursor, const struct pt_regs *regs,
                      bool allow_walk)
{
    x86_unwind_init_with_gs(cursor, regs, allow_walk, x86_read_gs_base());
}

__attribute__((no_stack_protector))
bool arch_unwind_return_query_pc(uintptr_t ret, uintptr_t *query_pc)
{
    if (ret == 0 || !query_pc) {
        return false;
    }
    *query_pc = ret - 1u;
    return true;
}
