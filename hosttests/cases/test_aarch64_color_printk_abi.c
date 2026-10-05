/*
 * test_aarch64_color_printk_abi.c — aarch64 M2/M3 plan Task 5.
 *
 * Proves the aarch64 color_printk stub matches the public ABI declared
 * in kernel/include/core/printk.h:53:
 *
 *   int color_printk(unsigned int FRcolor, unsigned int BKcolor,
 *                    const char *fmt, ...);
 *
 * Links the REAL kernel/arch/aarch64/runtime/printk_stub.c (production
 * file) against a kputs capture mock:
 *   1. compile-time: the TU includes <core/printk.h>; the stub must
 *      link against that exact prototype (the old void single-arg
 *      stub was a conflicting declaration).
 *   2. run-time: fg/bg set to garbage values must not be dereferenced
 *      as pointers (the stub ignores them).
 *   3. return value = byte length of fmt (kputs returns void, so the
 *      stub must compute it independently).
 */
#include "test_framework.h"
#include <core/printk.h>            /* expected signature */
#include <arch/aarch64/boot_log.h>  /* kputs mock shadow */
#include <string.h>

TEST_FUNC(test_color_printk_signature_matches_printk_h) {
    /* Compile-time verified by #include <core/printk.h> above; take a
     * function pointer at run time to confirm the ABI assignment. */
    int (*fp)(unsigned int, unsigned int, const char *, ...) = color_printk;
    assert_true(fp != NULL);
}

TEST_FUNC(test_color_printk_does_not_deref_colors) {
    /* Inject garbage fg/bg — must be ignored, not dereferenced. */
    mock_kputs_clear();
    int rc = color_printk(0xdeadbeefu, 0xcafebabeu, "M2-SLAB-ERR: ok\n");
    /* kputs returns void; color_printk must return the fmt byte length. */
    assert_eq(rc, (int)strlen("M2-SLAB-ERR: ok\n"));
    /* The mock received the literal fmt. */
    assert_true(strcmp(mock_kputs_last(), "M2-SLAB-ERR: ok\n") == 0);
}

TEST_FUNC(test_color_printk_varargs_ignored) {
    /* Variadic args consumed by a real formatter would change output;
     * the stub passes fmt through literally and still returns its
     * byte length (not the would-be formatted length). */
    mock_kputs_clear();
    int rc = color_printk(WHITE, BLACK, "%d %s", 1234, "str");
    assert_eq(rc, (int)strlen("%d %s"));
    assert_str_eq("%d %s", mock_kputs_last());
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_color_printk_signature_matches_printk_h),
    TEST_ENTRY(test_color_printk_does_not_deref_colors),
    TEST_ENTRY(test_color_printk_varargs_ignored),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
