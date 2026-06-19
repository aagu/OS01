#ifndef _KERNEL_TRACE_H
#define _KERNEL_TRACE_H

#include <kernel/arch/x86_64/regs.h>

#define KERNEL_TRACE_DEPTH 4

extern unsigned long kallsyms_addresses[] __attribute__((weak));
extern long kallsyms_syms_num __attribute__((weak));
extern long kallsyms_index[] __attribute__((weak));
extern char* kallsyms_names __attribute__((weak));

void backtrace(pt_regs_t* regs);
int32_t lookup_kallsyms(uint64_t address, int32_t level);

#endif