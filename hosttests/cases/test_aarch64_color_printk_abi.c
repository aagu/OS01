/*
 * test_aarch64_color_printk_abi.c — color_printk / serial_printk ABI + behaviour.
 *
 * Historical intent (aarch64 M2/M3 plan Task 5): prove the aarch64
 * color_printk matches the public ABI declared in
 * kernel/include/core/printk.h:
 *
 *   int color_printk(unsigned int FRcolor, unsigned int BKcolor,
 *                    const char *fmt, ...);
 *
 * The implementation used to live in an aarch64-only stub
 * (kernel/arch/aarch64/runtime/printk_stub.c).  master commit 42aef0fc
 * deleted that stub and the console became SHARED: kernel/core/printk.c
 * now supplies color_printk / serial_printk for BOTH x86_64 and aarch64
 * (kernel/Makefile's aarch64 KERNEL_C_SOURCES lists `core/printk.c`).
 *
 * This test therefore links the REAL shared kernel/core/printk.c:
 *   1. compile-time: the TU includes <core/printk.h>; the production TU
 *      is compiled against the same header, so any signature drift
 *      (e.g. the old `void color_printk(const char *fmt, ...)` stub) is
 *      a conflicting-types compile error.
 *   2. run-time: color_printk is a REAL formatter — it returns the
 *      FORMATTED byte count (vsprintf semantics), not the literal format
 *      string length the old stub returned.
 *   3. run-time: the FRcolor/BKcolor arguments are opaque colour values,
 *      never dereferenced as pointers (garbage values must not fault).
 *
 * The serial sink (write_serial_unlocked) is captured by
 * mock/aarch64_color_printk/printk_capture.c, which also maps the
 * framebuffer as absent so colour output falls back to serial — the same
 * observable path as an aarch64 boot before frame_buffer_init().
 */
#include "test_framework.h"
#include <core/printk.h>            /* expected signature */
#include <string.h>

/* Provided by mock/aarch64_color_printk/printk_capture.c. */
extern void        mock_serial_reset(void);
extern const char *mock_serial_buf(void);
extern size_t      mock_serial_len(void);

TEST_FUNC(test_printk_signature_matches_printk_h) {
    /* Compile-time verified by #include <core/printk.h> above; take
     * function pointers at run time to confirm the ABI assignment. */
    int (*cp)(unsigned int, unsigned int, const char *, ...) = color_printk;
    int (*sp)(const char *, ...) = serial_printk;
    assert_true(cp != NULL);
    assert_true(sp != NULL);
}

TEST_FUNC(test_serial_printk_formats_varargs_and_returns_length) {
    /* serial_printk formats its arguments and returns the formatted
     * byte count (not the literal `fmt` length). */
    mock_serial_reset();
    int rc = serial_printk("v=%d %s", 1234, "str");
    assert_eq(rc, (int)strlen("v=1234 str"));
    assert_str_eq("v=1234 str", mock_serial_buf());
    assert_eq((int)mock_serial_len(), (int)strlen("v=1234 str"));
}

TEST_FUNC(test_color_printk_formats_and_ignores_colors) {
    /* Inject garbage fg/bg — they are colour values, never
     * dereferenced.  Return value is the FORMATTED length. */
    mock_serial_reset();
    int rc = color_printk(0xdeadbeefu, 0xcafebabeu, "SLAB-ERR: %s\n", "ok");
    assert_eq(rc, (int)strlen("SLAB-ERR: ok\n"));
    assert_str_eq("SLAB-ERR: ok\n", mock_serial_buf());
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_printk_signature_matches_printk_h),
    TEST_ENTRY(test_serial_printk_formats_varargs_and_returns_length),
    TEST_ENTRY(test_color_printk_formats_and_ignores_colors),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
