#ifndef _KERNEL_ARCH_UNWIND_H
#define _KERNEL_ARCH_UNWIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pt_regs;

struct unwind_frame {
    uintptr_t next_fp;
    uintptr_t return_pc;
};

struct unwind_cursor {
    uintptr_t pc;
    uintptr_t sp;
    uintptr_t fp;
    uintptr_t stack_low;
    uintptr_t stack_high;
    size_t frame_align;
    bool kernel_origin;
};

void arch_unwind_init(struct unwind_cursor *cursor, const struct pt_regs *regs,
                      bool allow_walk);
bool arch_unwind_read_frame(uintptr_t fp, struct unwind_frame *out);
bool arch_unwind_return_query_pc(uintptr_t ret, uintptr_t *query_pc);

#endif
