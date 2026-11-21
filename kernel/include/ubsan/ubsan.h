// kernel/include/ubsan/ubsan.h — UBSan runtime ABI (compiler-rt compatible)
//
// Clang's -fsanitize=undefined emits calls to __ubsan_handle_* with
// descriptor structs whose layout is fixed by the compiler frontend (see
// clang/lib/CodeGen/CodeGenFunction::EmitCheckValue/TypeDescriptor).  This
// header is the single source for those layouts on the kernel side; the
// handlers live in kernel/ubsan/ubsan.c and are compiled WITHOUT
// instrumentation (object-specific rule in kernel/Makefile) so a report can
// never re-enter instrumentation.
//
// ABI reference: compiler-rt/lib/ubsan/ubsan_value.h (SourceLocation /
// TypeDescriptor) and ubsan_checks.inc handler signatures.  The kernel keeps
// the default "recoverable" symbol names (no _abort suffix): clang's driver
// rejects -fno-sanitize-recover=kernel-address and we want the same
// recover-then-panic shape for UBSan — the handler itself turns every
// violation into kpanic, so execution never actually continues.
//
// 本文件与 kernel/kasan/kasan.h 一起构成内核插桩运行时 ABI（roadmap P1
// UBSan + KASan）。
#ifndef _KERNEL_UBSAN_H
#define _KERNEL_UBSAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ValueHandle — the checked value as raw bits (compiler-rt calls this
// ValueHandle; width matches the pointer/word size).
typedef unsigned long ubsan_value_t;

// SourceLocation — { filename, line, column } exactly as emitted by clang;
// filename points into kernel .rodata (null when unavailable).
struct ubsan_source_location {
    const char *file;
    uint32_t line;
    uint32_t column;
};

// A zero SourceLocation means "no location available".
#define UBSAN_LOC_UNAVAILABLE(loc) ((loc) == 0 || (loc)->file == 0)

// TypeDescriptor — kind in [0, 2^16), name string follows inline (NUL
// terminated).  Kind 0xffff = integer, 0xfffe = float, 0x0000 = unnamed
// synthetic type (no name bytes).
struct ubsan_type_descriptor {
    uint16_t kind;
    uint16_t info;
    char name[];
};

// OverflowData — signed/unsigned arithmetic overflow and div-by-zero.
struct ubsan_overflow_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

// ShiftOutOfBoundsData.
struct ubsan_shift_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *lhs_type;
    const struct ubsan_type_descriptor *rhs_type;
};

// OutOfBoundsData — array-bounds / index checks.
struct ubsan_oob_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *array_type;
    const struct ubsan_type_descriptor *index_type;
};

// TypeMismatchData_v1 — null deref / misaligned / insufficient object size.
struct ubsan_type_mismatch_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
    unsigned char log_alignment;
    unsigned char type_check_kind;
};

// InvalidValueData — invalid bool / enum load.
struct ubsan_invalid_value_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

// VlaBoundData.
struct ubsan_vla_bound_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

// UnreachableData — builtin_unreachable and missing return.
struct ubsan_unreachable_data {
    struct ubsan_source_location loc;
};

// PointerOverflowData.
struct ubsan_pointer_overflow_data {
    struct ubsan_source_location loc;
};

// FloatCastOverflowData — handlers receive a bare pointer (compiler-rt
// hides the versioned layout behind void*).
struct ubsan_float_cast_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *from_type;
    const struct ubsan_type_descriptor *to_type;
};

// FunctionTypeMismatchData.
struct ubsan_function_mismatch_data {
    struct ubsan_source_location loc;
    const struct ubsan_type_descriptor *type;
};

// InvalidBuiltinData.
struct ubsan_invalid_builtin_data {
    struct ubsan_source_location loc;
    unsigned char kind;
};

// NonNullArgData.
struct ubsan_nonnull_arg_data {
    struct ubsan_source_location loc;
    uint32_t arg_index;
};

// Report entry used by tests and the KASan path: formats one violation line
// through log_err and panics.  error is a short tag ("add overflow", ...).
void ubsan_report(const struct ubsan_source_location *loc, const char *error,
                  const char *type_name, unsigned long value);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_UBSAN_H */
