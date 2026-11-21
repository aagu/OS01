#include <core/trace.h>
#include <core/trace_symbols.h>
#include <arch/unwind.h>
#include <arch/early_print.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static void trace_put_hex64(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    arch_early_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        arch_early_putc(hex[(val >> shift) & 0xfU]);
    }
}

static void trace_put_dec_pad4(uint64_t val)
{
    char buf[24];
    int len = 0;

    if (val == 0) {
        arch_early_puts("0000");
        return;
    }
    while (val != 0 && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (int)(val % 10U));
        val /= 10U;
    }
    for (int i = len; i < 4; i++) {
        arch_early_putc('0');
    }
    while (len > 0) {
        arch_early_putc(buf[--len]);
    }
}

static void trace_print_frame(int level, uintptr_t display_pc,
                              uintptr_t query_pc, bool lookup_sym)
{
    for (int i = 0; i < level * 2; i++) {
        arch_early_putc(' ');
    }
    arch_early_puts("+---> address:");
    trace_put_hex64((uint64_t)display_pc);

    if (lookup_sym) {
        const char *name = NULL;
        uint64_t offset = 0;
        if (trace_symbol_lookup(query_pc, &name, &offset) && name != NULL) {
            arch_early_puts(" \t(+) ");
            trace_put_dec_pad4(offset);
            arch_early_puts(" function:");
            arch_early_puts(name);
            arch_early_putc('\n');
            return;
        }
    }
    arch_early_puts(" \t(no symbol)\n");
}

static bool fp_in_range(const struct unwind_cursor *cursor, uintptr_t fp)
{
    if (cursor->frame_align == 0 || (cursor->frame_align & (cursor->frame_align - 1u)) != 0) {
        return false;
    }
    if ((fp & (cursor->frame_align - 1u)) != 0) {
        return false;
    }
    if ((fp & (sizeof(uintptr_t) - 1u)) != 0) {
        return false;
    }
    if (fp < cursor->stack_low || fp < cursor->sp) {
        return false;
    }
    if (fp >= cursor->stack_high) {
        return false;
    }
    if (fp > UINTPTR_MAX - sizeof(struct unwind_frame)) {
        return false;
    }
    if ((cursor->stack_high - fp) < sizeof(struct unwind_frame)) {
        return false;
    }
    return true;
}

__attribute__((no_stack_protector))
void trace_exception(const struct pt_regs *regs, enum trace_mode mode)
{
    struct unwind_cursor cursor;
    arch_unwind_init(&cursor, regs, mode == TRACE_FULL);

    arch_early_puts("====================== Kernel Stack Backtrace ======================\n");
    trace_print_frame(0, cursor.pc, cursor.pc, cursor.kernel_origin);

    if (mode != TRACE_FULL || !cursor.kernel_origin ||
        cursor.stack_low >= cursor.stack_high) {
        goto out;
    }
    if (cursor.sp < cursor.stack_low || cursor.sp >= cursor.stack_high) {
        goto out;
    }

    uintptr_t fp = cursor.fp;
    for (int level = 1; level < KERNEL_TRACE_DEPTH; level++) {
        if (!fp_in_range(&cursor, fp)) {
            break;
        }

        struct unwind_frame frame;
        if (!arch_unwind_read_frame(fp, &frame)) {
            break;
        }

        uintptr_t query_pc = 0;
        if (!arch_unwind_return_query_pc(frame.return_pc, &query_pc)) {
            break;
        }
        if (!trace_kernel_text_contains(query_pc)) {
            break;
        }

        trace_print_frame(level, frame.return_pc, query_pc, true);

        if (frame.next_fp == 0) {
            break;
        }
        if (fp > UINTPTR_MAX - sizeof(struct unwind_frame) ||
            frame.next_fp < fp + sizeof(struct unwind_frame)) {
            break;
        }
        fp = frame.next_fp;
    }

out:
    arch_early_puts("====================== End Kernel Stack Backtrace ======================\n");
}
