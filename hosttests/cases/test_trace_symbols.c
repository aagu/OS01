/* @os01-test
 * id: test_trace_symbols
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: pr
 * scenarios: []
 */
#include <test_framework.h>
#include <core/trace_symbols.h>

#if !defined(TRACE_SYMBOLS_FIXTURE_EMPTY) && !defined(TRACE_SYMBOLS_FIXTURE_MISSING)
#include <sys/wait.h>
#include <unistd.h>
#endif

extern const char __trace_text_start[];
extern const char __trace_text_end[];
extern const char __trace_ltext_start[];
extern const char __trace_ltext_end[];
extern const char sym_seg1_first[];
extern const char sym_seg1_last[];
extern const char sym_seg2_first[];

__asm__(
    ".section .rodata\n"
    ".align 16\n"
    ".globl __trace_text_start\n"
    "__trace_text_start:\n"
    ".globl sym_seg1_first\n"
    "sym_seg1_first:\n"
    "    .space 16\n"
    ".globl sym_seg1_last\n"
    "sym_seg1_last:\n"
    "    .space 16\n"
    ".globl __trace_text_end\n"
    "__trace_text_end:\n"
    "    .space 64\n"
    ".globl __trace_ltext_start\n"
    "__trace_ltext_start:\n"
    "    .space 16\n"
    ".globl sym_seg2_first\n"
    "sym_seg2_first:\n"
    "    .space 16\n"
    ".globl __trace_ltext_end\n"
    "__trace_ltext_end:\n"
    "    .space 16\n"
);

#if defined(TRACE_SYMBOLS_FIXTURE_EMPTY)

__asm__(
    ".section .rodata\n"
    ".globl kallsyms_addresses\n"
    ".align 8\n"
    "kallsyms_addresses:\n"
    ".globl kallsyms_syms_num\n"
    ".align 8\n"
    "kallsyms_syms_num:\n"
    "    .quad 0\n"
    ".globl kallsyms_index\n"
    ".align 8\n"
    "kallsyms_index:\n"
    ".globl kallsyms_names\n"
    ".align 8\n"
    "kallsyms_names:\n"
);

TEST_FUNC(test_trace_symbol_empty_fixture)
{
    const char *sentinel_name = "sentinel";
    const char *name = sentinel_name;
    uint64_t off = UINT64_C(0xdeadbeef);

    assert_true(trace_kernel_text_contains((uintptr_t)sym_seg1_first));
    assert_false(trace_symbol_lookup((uintptr_t)sym_seg1_first, &name, &off));
    assert_true(name == sentinel_name);
    assert_eq(UINT64_C(0xdeadbeef), off);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_trace_symbol_empty_fixture),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("trace-symbols-empty");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

#elif defined(TRACE_SYMBOLS_FIXTURE_MISSING)

TEST_FUNC(test_trace_symbol_missing_fixture)
{
    const char *sentinel_name = "sentinel";
    const char *name = sentinel_name;
    uint64_t off = UINT64_C(0xcafebabe);

    assert_true(trace_kernel_text_contains((uintptr_t)sym_seg1_first));
    assert_false(trace_symbol_lookup((uintptr_t)sym_seg1_first, &name, &off));
    assert_true(name == sentinel_name);
    assert_eq(UINT64_C(0xcafebabe), off);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_trace_symbol_missing_fixture),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("trace-symbols-missing");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

#else

__asm__(
    ".section .rodata\n"
    ".globl kallsyms_addresses\n"
    ".align 8\n"
    "kallsyms_addresses:\n"
    "    .quad sym_seg1_first\n"
    "    .quad sym_seg1_last\n"
    "    .quad sym_seg2_first\n"
    ".globl kallsyms_syms_num\n"
    ".align 8\n"
    "kallsyms_syms_num:\n"
    "    .quad 3\n"
    ".globl kallsyms_index\n"
    ".align 8\n"
    "kallsyms_index:\n"
    "    .quad 0\n"
    "    .quad 15\n"
    "    .quad 29\n"
    ".globl kallsyms_names\n"
    ".align 8\n"
    "kallsyms_names:\n"
    "    .asciz \"sym_seg1_first\"\n"
    "    .asciz \"sym_seg1_last\"\n"
    "    .asciz \"sym_seg2_first\"\n"
);

static int run_companion_elf(const char *path)
{
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd), "%s >/dev/null", path);
    if (n < 0 || (size_t)n >= sizeof(cmd))
        return -1;
    int status = system(cmd);
    if (status == -1 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

TEST_FUNC(test_trace_symbol_boundaries)
{
    const char *sentinel_name = "unchanged";
    const char *name = sentinel_name;
    uint64_t off = UINT64_C(0x12345678);

    /* Range containment */
    assert_true(trace_kernel_text_contains((uintptr_t)__trace_text_start));
    assert_true(trace_kernel_text_contains((uintptr_t)__trace_text_end - 1));
    assert_false(trace_kernel_text_contains((uintptr_t)__trace_text_end));
    assert_false(trace_kernel_text_contains((uintptr_t)__trace_text_end + 8));
    assert_true(trace_kernel_text_contains((uintptr_t)__trace_ltext_start));
    assert_false(trace_kernel_text_contains((uintptr_t)__trace_ltext_end));

    /* Exact first symbol -> offset 0 */
    assert_true(trace_symbol_lookup((uintptr_t)sym_seg1_first, &name, &off));
    assert_str_eq("sym_seg1_first", name);
    assert_eq(0, off);

    /* Last symbol in segment 1 resolves up to end - 1 */
    assert_true(trace_symbol_lookup((uintptr_t)__trace_text_end - 1, &name, &off));
    assert_str_eq("sym_seg1_last", name);
    assert_eq((uintptr_t)__trace_text_end - 1 - (uintptr_t)sym_seg1_last, off);

    /* Segment 1 end fails without modifying outputs */
    name = sentinel_name;
    off = UINT64_C(0x12345678);
    assert_false(trace_symbol_lookup((uintptr_t)__trace_text_end, &name, &off));
    assert_true(name == sentinel_name);
    assert_eq(UINT64_C(0x12345678), off);

    /* Gap between segments fails without modifying outputs */
    assert_false(trace_symbol_lookup((uintptr_t)__trace_text_end + 8, &name, &off));
    assert_true(name == sentinel_name);
    assert_eq(UINT64_C(0x12345678), off);

    /* Address in segment 2 before sym_seg2_first must NOT borrow sym_seg1_last */
    assert_false(trace_symbol_lookup((uintptr_t)__trace_ltext_start, &name, &off));
    assert_true(name == sentinel_name);
    assert_eq(UINT64_C(0x12345678), off);

    /* First symbol in segment 2 */
    assert_true(trace_symbol_lookup((uintptr_t)sym_seg2_first, &name, &off));
    assert_str_eq("sym_seg2_first", name);
    assert_eq(0, off);
}

TEST_FUNC(test_trace_symbol_empty)
{
    assert_eq(0, run_companion_elf(TRACE_SYMBOLS_EMPTY_ELF));
}

TEST_FUNC(test_trace_symbol_missing)
{
    assert_eq(0, run_companion_elf(TRACE_SYMBOLS_MISSING_ELF));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_trace_symbol_boundaries),
    TEST_ENTRY(test_trace_symbol_empty),
    TEST_ENTRY(test_trace_symbol_missing),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("trace-symbols");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}

#endif
