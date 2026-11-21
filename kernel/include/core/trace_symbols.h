#ifndef _KERNEL_TRACE_SYMBOLS_H
#define _KERNEL_TRACE_SYMBOLS_H

#include <stdbool.h>
#include <stdint.h>

struct trace_text_range {
    uintptr_t start;
    uintptr_t end;
};

extern const uint64_t kallsyms_addresses[] __attribute__((weak));
extern const uint64_t kallsyms_syms_num __attribute__((weak));
extern const uint64_t kallsyms_index[] __attribute__((weak));
extern const char kallsyms_names[] __attribute__((weak));

extern const char __trace_text_start[];
extern const char __trace_text_end[];
extern const char __trace_ltext_start[];
extern const char __trace_ltext_end[];

bool trace_kernel_text_contains(uintptr_t pc);
bool trace_symbol_lookup(uintptr_t query_pc, const char **name, uint64_t *offset);

#endif
