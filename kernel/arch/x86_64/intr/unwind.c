#include <arch/unwind.h>
#include <arch/x86_64/unwind.h>
#include <stddef.h>

extern const char x86_unwind_load_next_fp[];
extern const char x86_unwind_load_ret_pc[];
extern const char x86_unwind_load_fail[];

static inline bool x86_is_canonical_va(uintptr_t addr)
{
    int64_t top = (int64_t)addr >> 47;
    return top == 0 || top == -1;
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
