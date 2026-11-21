#ifndef _KERNEL_TRACE_H
#define _KERNEL_TRACE_H

#include <arch/thread.h>
#include <core/trace_symbols.h>

#define KERNEL_TRACE_DEPTH 4

void backtrace(pt_regs_t* regs);
int32_t lookup_kallsyms(uint64_t address, int32_t level);

#endif
