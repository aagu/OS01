#ifndef _KERNEL_ARCH_AARCH64_UNWIND_H
#define _KERNEL_ARCH_AARCH64_UNWIND_H

#include <stdbool.h>
#include <stdint.h>
#include <arch/regs.h>
#include <arch/unwind.h>

bool aarch64_unwind_load_pair(uintptr_t addr, struct unwind_frame *out);
bool aarch64_unwind_fixup(struct pt_regs *regs, uint64_t esr, uintptr_t far);
void aarch64_unwind_publish_task_stack(unsigned cpu, uintptr_t base);

bool aarch64_unwind_resolve_stack(uintptr_t tpidr, uintptr_t sp,
                                  uintptr_t *low_out, uintptr_t *high_out);
void aarch64_unwind_init_with_tpidr(struct unwind_cursor *cursor,
                                    const struct pt_regs *regs,
                                    bool allow_walk,
                                    uintptr_t tpidr);

#endif
