// kernel/ubsan/ubsan.c — UBSan runtime handlers (x86_64 first, arch-neutral)
//
// Called by code clang emitted under -fsanitize=undefined (KERNEL_UBSAN=1).
// Every violation is reported through log_err with the source location and
// the offending type, then kpanic — the kernel never continues past an
// undefined-behavior check.  Built WITHOUT instrumentation via the
// object-specific rule in kernel/Makefile (a handler that reports UB must
// not be able to trigger UB reporting itself).
//
// Handler signatures follow compiler-rt/lib/ubsan/ubsan_handlers.cpp; the
// descriptor structs are defined in <ubsan/ubsan.h>.  Both the plain
// (recoverable) and the _abort symbol spellings are provided so a future
// -fno-sanitize-recover build links unchanged.

#include <ubsan/ubsan.h>

#include <stddef.h>              /* NULL */

#include <log/log.h>
#include <core/panic.h>

// ── report formatting ────────────────────────────────────────

// The type descriptor carries the spelled-out type name inline (NUL
// terminated) after the 4-byte header when kind != 0.  Integer descriptors
// (kind 0xffff) also have a name.  Synthetic unnamed descriptors (kind 0)
// have no name bytes — return a placeholder.
static const char *ubsan_type_name(const struct ubsan_type_descriptor *type)
{
    if (type == NULL || type->kind == 0)
        return "(unknown)";
    return type->name;
}

void ubsan_report(const struct ubsan_source_location *loc, const char *error,
                  const char *type_name, unsigned long value)
{
    if (loc != NULL && loc->file != NULL) {
        log_err("ubsan: %s at %s:%u:%u type=%s value=%#lx\n",
                error, loc->file, loc->line, loc->column, type_name, value);
    } else {
        log_err("ubsan: %s at (unknown location) type=%s value=%#lx\n",
                error, type_name, value);
    }
    kpanic("ubsan: undefined behavior detected\n");
}

// ── arithmetic overflow ──────────────────────────────────────

void __ubsan_handle_add_overflow(struct ubsan_overflow_data *data,
                                 unsigned long lhs, unsigned long rhs)
{
    (void)rhs;
    ubsan_report(&data->loc, "signed integer add overflow",
                 ubsan_type_name(data->type), lhs);
}

void __ubsan_handle_sub_overflow(struct ubsan_overflow_data *data,
                                 unsigned long lhs, unsigned long rhs)
{
    (void)rhs;
    ubsan_report(&data->loc, "signed integer sub overflow",
                 ubsan_type_name(data->type), lhs);
}

void __ubsan_handle_mul_overflow(struct ubsan_overflow_data *data,
                                 unsigned long lhs, unsigned long rhs)
{
    (void)rhs;
    ubsan_report(&data->loc, "signed integer mul overflow",
                 ubsan_type_name(data->type), lhs);
}

void __ubsan_handle_negate_overflow(struct ubsan_overflow_data *data,
                                    unsigned long old_val)
{
    ubsan_report(&data->loc, "signed integer negation overflow",
                 ubsan_type_name(data->type), old_val);
}

// Divide-by-zero and division overflow share one handler.
void __ubsan_handle_divrem_overflow(struct ubsan_overflow_data *data,
                                    unsigned long lhs, unsigned long rhs)
{
    (void)lhs;
    ubsan_report(&data->loc, "division by zero or overflow",
                 ubsan_type_name(data->type), rhs);
}

// ── shift ────────────────────────────────────────────────────

void __ubsan_handle_shift_out_of_bounds(struct ubsan_shift_data *data,
                                        unsigned long lhs, unsigned long rhs)
{
    (void)lhs;
    ubsan_report(&data->loc, "shift out of bounds",
                 ubsan_type_name(data->lhs_type), rhs);
}

// ── bounds / index ───────────────────────────────────────────

void __ubsan_handle_out_of_bounds(struct ubsan_oob_data *data,
                                  unsigned long index)
{
    ubsan_report(&data->loc, "index out of bounds",
                 ubsan_type_name(data->index_type), index);
}

// ── type mismatch (null deref / misalignment / object-size) ──

void __ubsan_handle_type_mismatch_v1(struct ubsan_type_mismatch_data *data,
                                     unsigned long ptr)
{
    const char *error;
    if (ptr == 0)
        error = "null pointer access";
    else if (data->log_alignment != 0 &&
             (ptr & ((1UL << data->log_alignment) - 1)) != 0)
        error = "misaligned pointer access";
    else
        error = "object size too small for access";
    ubsan_report(&data->loc, error, ubsan_type_name(data->type), ptr);
}

// Pre-v1 spelling kept so a compiler that still emits the legacy name
// links (same descriptor layout, alignment field unused).
void __ubsan_handle_type_mismatch(struct ubsan_type_mismatch_data *data,
                                  unsigned long ptr)
{
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

// ── invalid values ───────────────────────────────────────────

void __ubsan_handle_load_invalid_value(struct ubsan_invalid_value_data *data,
                                       unsigned long val)
{
    ubsan_report(&data->loc, "invalid bool/enum load",
                 ubsan_type_name(data->type), val);
}

// ── VLA ──────────────────────────────────────────────────────

void __ubsan_handle_vla_bound_not_positive(struct ubsan_vla_bound_data *data,
                                           unsigned long bound)
{
    ubsan_report(&data->loc, "VLA bound not positive",
                 ubsan_type_name(data->type), bound);
}

// ── control flow ─────────────────────────────────────────────

void __ubsan_handle_builtin_unreachable(struct ubsan_unreachable_data *data)
{
    ubsan_report(&data->loc, "execution reached unreachable code", "", 0);
}

void __ubsan_handle_missing_return(struct ubsan_unreachable_data *data)
{
    ubsan_report(&data->loc, "non-void function returned without a value", "", 0);
}

// ── pointer arithmetic ───────────────────────────────────────

void __ubsan_handle_pointer_overflow(struct ubsan_pointer_overflow_data *data,
                                     unsigned long base, unsigned long result)
{
    (void)base;
    ubsan_report(&data->loc, "pointer arithmetic overflow", "", result);
}

// ── float / function / builtin / nonnull (defensive) ─────────
// The kernel builds with -mno-80387 -mno-sse so FP casts should never
// occur; these handlers exist so the symbols resolve if a future check
// group (function, nonnull-attribute, ...) starts emitting calls.

void __ubsan_handle_float_cast_overflow(void *data, unsigned long from)
{
    (void)from;
    struct ubsan_float_cast_data *d = (struct ubsan_float_cast_data *)data;
    ubsan_report(&d->loc, "float cast overflow", "(float)", from);
}

void __ubsan_handle_function_type_mismatch(
    struct ubsan_function_mismatch_data *data, unsigned long fptr)
{
    ubsan_report(&data->loc, "indirect call through mismatched function pointer",
                 ubsan_type_name(data->type), fptr);
}

void __ubsan_handle_invalid_builtin(struct ubsan_invalid_builtin_data *data)
{
    ubsan_report(&data->loc, "call to invalid builtin", "", data->kind);
}

void __ubsan_handle_nonnull_arg(struct ubsan_nonnull_arg_data *data)
{
    ubsan_report(&data->loc, "null pointer passed to nonnull argument",
                 "", data->arg_index);
}

// ── _abort spellings ─────────────────────────────────────────
// -fno-sanitize-recover=undefined makes clang emit _abort-suffixed calls.
// We default to the recoverable spellings above, but provide the abort
// variants as aliases so a future hardening build links unchanged.

void __ubsan_handle_add_overflow_abort(struct ubsan_overflow_data *data,
                                       unsigned long lhs, unsigned long rhs)
    __attribute__((alias("__ubsan_handle_add_overflow")));
void __ubsan_handle_sub_overflow_abort(struct ubsan_overflow_data *data,
                                       unsigned long lhs, unsigned long rhs)
    __attribute__((alias("__ubsan_handle_sub_overflow")));
void __ubsan_handle_mul_overflow_abort(struct ubsan_overflow_data *data,
                                       unsigned long lhs, unsigned long rhs)
    __attribute__((alias("__ubsan_handle_mul_overflow")));
void __ubsan_handle_negate_overflow_abort(struct ubsan_overflow_data *data,
                                          unsigned long old_val)
    __attribute__((alias("__ubsan_handle_negate_overflow")));
void __ubsan_handle_divrem_overflow_abort(struct ubsan_overflow_data *data,
                                          unsigned long lhs, unsigned long rhs)
    __attribute__((alias("__ubsan_handle_divrem_overflow")));
void __ubsan_handle_shift_out_of_bounds_abort(struct ubsan_shift_data *data,
                                              unsigned long lhs, unsigned long rhs)
    __attribute__((alias("__ubsan_handle_shift_out_of_bounds")));
void __ubsan_handle_out_of_bounds_abort(struct ubsan_oob_data *data,
                                        unsigned long index)
    __attribute__((alias("__ubsan_handle_out_of_bounds")));
void __ubsan_handle_type_mismatch_v1_abort(struct ubsan_type_mismatch_data *data,
                                           unsigned long ptr)
    __attribute__((alias("__ubsan_handle_type_mismatch_v1")));
void __ubsan_handle_load_invalid_value_abort(struct ubsan_invalid_value_data *data,
                                             unsigned long val)
    __attribute__((alias("__ubsan_handle_load_invalid_value")));
void __ubsan_handle_vla_bound_not_positive_abort(struct ubsan_vla_bound_data *data,
                                                 unsigned long bound)
    __attribute__((alias("__ubsan_handle_vla_bound_not_positive")));
void __ubsan_handle_builtin_unreachable_abort(struct ubsan_unreachable_data *data)
    __attribute__((alias("__ubsan_handle_builtin_unreachable")));
void __ubsan_handle_missing_return_abort(struct ubsan_unreachable_data *data)
    __attribute__((alias("__ubsan_handle_missing_return")));
void __ubsan_handle_pointer_overflow_abort(struct ubsan_pointer_overflow_data *data,
                                           unsigned long base, unsigned long result)
    __attribute__((alias("__ubsan_handle_pointer_overflow")));
void __ubsan_handle_float_cast_overflow_abort(void *data, unsigned long from)
    __attribute__((alias("__ubsan_handle_float_cast_overflow")));
void __ubsan_handle_function_type_mismatch_abort(
    struct ubsan_function_mismatch_data *data, unsigned long fptr)
    __attribute__((alias("__ubsan_handle_function_type_mismatch")));
void __ubsan_handle_invalid_builtin_abort(struct ubsan_invalid_builtin_data *data)
    __attribute__((alias("__ubsan_handle_invalid_builtin")));
void __ubsan_handle_nonnull_arg_abort(struct ubsan_nonnull_arg_data *data)
    __attribute__((alias("__ubsan_handle_nonnull_arg")));
