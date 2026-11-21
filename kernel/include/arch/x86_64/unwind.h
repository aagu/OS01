#ifndef _KERNEL_ARCH_X86_64_UNWIND_H
#define _KERNEL_ARCH_X86_64_UNWIND_H

#include <stdbool.h>
#include <stdint.h>
#include <arch/regs.h>
#include <arch/unwind.h>

bool x86_unwind_load_pair(uintptr_t addr, struct unwind_frame *out);
bool x86_unwind_fixup(struct pt_regs *regs, uint64_t error, uintptr_t cr2);

bool x86_unwind_resolve_stack(uintptr_t gs_base, uintptr_t sp,
                              uintptr_t *low_out, uintptr_t *high_out);
void x86_unwind_init_with_gs(struct unwind_cursor *cursor,
                             const struct pt_regs *regs,
                             bool allow_walk,
                             uintptr_t gs_base);

#endif
