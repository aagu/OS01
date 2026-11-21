#include <arch/unwind.h>
#include <arch/aarch64/unwind.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/boot_layout.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <stddef.h>

extern const char aarch64_unwind_load_next_fp[];
extern const char aarch64_unwind_load_ret_pc[];
extern const char aarch64_unwind_load_fail[];
extern uint64_t aarch64_boot_stack_top_addr(uint32_t cpu);

static uintptr_t aarch64_task_stack_bases[NR_CPUS];

static inline bool aarch64_is_valid_fsc(uint64_t fsc)
{
    return (fsc >= 0x04ULL && fsc <= 0x07ULL) ||
           (fsc >= 0x09ULL && fsc <= 0x0bULL) ||
           (fsc >= 0x0dULL && fsc <= 0x0fULL);
}

__attribute__((no_stack_protector))
bool aarch64_unwind_fixup(struct pt_regs *regs, uint64_t esr, uintptr_t far)
{
    if (!regs) {
        return false;
    }
    if ((regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL1H) {
        return false;
    }
    uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
    if (ec != AARCH64_EC_DATA_ABORT_SAME) {
        return false;
    }
    if ((esr & (AARCH64_ESR_FNV_BIT | AARCH64_ESR_ISS_WNR |
                AARCH64_ESR_ISS_CM | AARCH64_ESR_ISS_S1PTW)) != 0) {
        return false;
    }
    uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;
    if (!aarch64_is_valid_fsc(fsc)) {
        return false;
    }

    uintptr_t elr = (uintptr_t)regs->elr_el1;
    uintptr_t base = (uintptr_t)regs->x0;
    if (elr == (uintptr_t)aarch64_unwind_load_next_fp) {
        if (far != base) {
            return false;
        }
    } else if (elr == (uintptr_t)aarch64_unwind_load_ret_pc) {
        if (base > UINTPTR_MAX - 8u || far != base + 8u) {
            return false;
        }
    } else {
        return false;
    }

    regs->elr_el1 = (uint64_t)(uintptr_t)aarch64_unwind_load_fail;
    return true;
}

__attribute__((no_stack_protector))
void aarch64_unwind_publish_task_stack(unsigned cpu, uintptr_t base)
{
    if (cpu < NR_CPUS) {
        __atomic_store_n(&aarch64_task_stack_bases[cpu], base, __ATOMIC_RELEASE);
    }
}

__attribute__((no_stack_protector))
bool aarch64_unwind_resolve_stack(uintptr_t tpidr, uintptr_t sp,
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
    if (tpidr < pcpu_base || tpidr >= pcpu_end) {
        return false;
    }
    uintptr_t delta = tpidr - pcpu_base;
    if ((delta % sizeof(percpu_t)) != 0) {
        return false;
    }
    unsigned cpu = (unsigned)(delta / sizeof(percpu_t));
    if (cpu >= NR_CPUS) {
        return false;
    }

    uintptr_t task_base = __atomic_load_n(&aarch64_task_stack_bases[cpu], __ATOMIC_ACQUIRE);
    uintptr_t cand_low = 0;
    uintptr_t cand_high = 0;

    if (task_base != 0) {
        if (task_base < (uintptr_t)AARCH64_PAGE_OFFSET || (task_base & 15u) != 0) {
            return false;
        }
        cand_low = (task_base + sizeof(task_t) + 256u + 15u) & ~(uintptr_t)15u;
        cand_high = task_base + STACK_SIZE;
    } else {
        uint64_t boot_top_pa = aarch64_boot_stack_top_addr((uint32_t)cpu);
        uintptr_t boot_top = (uintptr_t)(AARCH64_PAGE_OFFSET + boot_top_pa);
        if (boot_top < (uintptr_t)AARCH64_PAGE_OFFSET + 4096u) {
            return false;
        }
        cand_high = boot_top;
        cand_low = boot_top - 4096u;
    }

    if (cand_low >= cand_high || sp < cand_low || sp >= cand_high) {
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
void aarch64_unwind_init_with_tpidr(struct unwind_cursor *cursor,
                                    const struct pt_regs *regs,
                                    bool allow_walk,
                                    uintptr_t tpidr)
{
    if (!cursor || !regs) {
        return;
    }

    bool is_el1h = ((regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) == AARCH64_SPSR_MODE_EL1H);
    cursor->pc = (uintptr_t)regs->elr_el1;
    cursor->sp = is_el1h ? ((uintptr_t)regs + sizeof(struct pt_regs)) : (uintptr_t)regs->sp_el0;
    cursor->fp = (uintptr_t)regs->x29;
    cursor->stack_low = 0;
    cursor->stack_high = 0;
    cursor->frame_align = sizeof(uintptr_t);
    cursor->kernel_origin = is_el1h;

    if (!allow_walk || !is_el1h) {
        return;
    }

    uintptr_t low = 0;
    uintptr_t high = 0;
    if (aarch64_unwind_resolve_stack(tpidr, cursor->sp, &low, &high)) {
        cursor->stack_low = low;
        cursor->stack_high = high;
    }
}

__attribute__((no_stack_protector))
void arch_unwind_init(struct unwind_cursor *cursor, const struct pt_regs *regs,
                      bool allow_walk)
{
    uintptr_t tpidr = 0;
#if defined(__aarch64__) && !defined(OS01_HOST_TEST)
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(tpidr));
#endif
    aarch64_unwind_init_with_tpidr(cursor, regs, allow_walk, tpidr);
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
#if !defined(OS01_HOST_TEST)
    if (fp < (uintptr_t)AARCH64_PAGE_OFFSET) {
        return false;
    }
#endif
    return aarch64_unwind_load_pair(fp, out);
}

__attribute__((no_stack_protector))
bool arch_unwind_return_query_pc(uintptr_t ret, uintptr_t *query_pc)
{
    if (!query_pc || ret < 4u || (ret & 3u) != 0) {
        return false;
    }
    *query_pc = ret - 4u;
    return true;
}
