#ifndef _KERNEL_TRACE_H
#define _KERNEL_TRACE_H

#include <arch/regs.h>
#include <core/trace_symbols.h>

#define KERNEL_TRACE_DEPTH 4

enum trace_mode {
    TRACE_FULL,
    TRACE_PC_ONLY,
};

void trace_exception(const struct pt_regs *regs, enum trace_mode mode);

#endif
