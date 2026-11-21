#include <core/trace_symbols.h>
#include <stddef.h>

static bool range_contains(const char *start_sym, const char *end_sym,
                           uintptr_t pc, struct trace_text_range *out)
{
    if (!start_sym || !end_sym) {
        return false;
    }
    uintptr_t start = (uintptr_t)start_sym;
    uintptr_t end = (uintptr_t)end_sym;
    if (start >= end) {
        return false;
    }
    if (pc < start || pc >= end) {
        return false;
    }
    if (out) {
        out->start = start;
        out->end = end;
    }
    return true;
}

static bool find_text_range(uintptr_t pc, struct trace_text_range *out)
{
    if (range_contains(__trace_text_start, __trace_text_end, pc, out)) {
        return true;
    }
    if (range_contains(__trace_ltext_start, __trace_ltext_end, pc, out)) {
        return true;
    }
    return false;
}

bool trace_kernel_text_contains(uintptr_t pc)
{
    return find_text_range(pc, NULL);
}

bool trace_symbol_lookup(uintptr_t query_pc, const char **name, uint64_t *offset)
{
    struct trace_text_range seg;
    if (!find_text_range(query_pc, &seg)) {
        return false;
    }

    if (!kallsyms_addresses || !&kallsyms_syms_num ||
        !kallsyms_index || !kallsyms_names) {
        return false;
    }

    uint64_t count = kallsyms_syms_num;
    if (count == 0) {
        return false;
    }

    uint64_t lo = 0;
    uint64_t hi = count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (kallsyms_addresses[mid] <= (uint64_t)query_pc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return false;
    }

    uint64_t idx = lo - 1;
    uint64_t sym_addr = kallsyms_addresses[idx];
    if (sym_addr < (uint64_t)seg.start || sym_addr >= (uint64_t)seg.end) {
        return false;
    }

    if (name) {
        *name = &kallsyms_names[kallsyms_index[idx]];
    }
    if (offset) {
        *offset = (uint64_t)query_pc - sym_addr;
    }
    return true;
}
