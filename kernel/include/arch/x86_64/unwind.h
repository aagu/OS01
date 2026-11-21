#ifndef _KERNEL_ARCH_X86_64_UNWIND_H
#define _KERNEL_ARCH_X86_64_UNWIND_H

#include <stdbool.h>
#include <stdint.h>
#include <arch/regs.h>
#include <arch/unwind.h>

bool x86_unwind_load_pair(uintptr_t addr, struct unwind_frame *out);
bool x86_unwind_fixup(struct pt_regs *regs, uint64_t error, uintptr_t cr2);

#endif
