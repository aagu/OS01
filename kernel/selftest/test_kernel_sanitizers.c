// kernel/selftest/test_kernel_sanitizers.c — sanitizer probe fixture
//
// Compiled ONLY when KERNEL_SANITIZER_TEST=ubsan|kasan (see kernel/Makefile:
// source gated like the fault_probe fixtures, no symbols in the normal
// build).  Called from kernel_main after device bring-up via the same
// pattern the CAPACITY_TEST fixture uses.
//
// Each case logs a distinctive armed-marker on serial, then triggers ONE
// genuine compiler-instrumented violation and lets the runtime report +
// kpanic.  The QEMU E2E suite (qemutests/test_kernel_sanitizers.py) asserts
// the marker AND the runtime's report line on the serial log.

#include <memory/slab.h>
#include <log/log.h>

// ── UBSan case: signed integer overflow ──────────────────────
// volatile defeats constant folding, so clang -fsanitize=undefined emits a
// real __ubsan_handle_sub_overflow call.  The handler (kernel/ubsan/ubsan.c)
// logs "ubsan: signed integer sub overflow at <file>:<line>" and kpanics.
static void sanitizer_probe_ubsan(void)
{
    volatile int value = 0x7fffffff;

    log_info("[san-probe] armed:ubsan\n");
    value = value + 1; // UB: INT_MAX + 1 signed overflow
    // Unreached: the UBSan handler panics.
    log_info("[san-probe] ubsan did not trip (value=%d)\n", (int)value);
}

// ── KASan case: heap redzone write ───────────────────────────
// kmalloc(24) lands in the 32-byte cache; the payload redzone [24, 32) is
// poisoned 0xfc by the slab hook, so the volatile write below trips an
// inline shadow check → __asan_report_store1_noabort → kasan_report_access
// logs "kasan: WRITE of 1 byte(s) at ..." and kpanics.
static void sanitizer_probe_kasan(void)
{
    unsigned char *ptr = (unsigned char *)kmalloc(24);

    if (ptr == NULL) {
        log_err("[san-probe] kasan arm failed: kmalloc(24) == NULL\n");
        return;
    }

    log_info("[san-probe] armed:kasan ptr=%#lx\n", (unsigned long)ptr);
    volatile unsigned char *q = ptr;
    q[28] = 0x41; // 4 bytes past the requested size, inside the redzone
    // Unreached: the KASan report panics.
    log_info("[san-probe] kasan did not trip\n");
}

// Dispatched from kernel_main under the KERNEL_SANITIZER_TEST_* macros.
void sanitizer_probe_run(void)
{
#if defined(KERNEL_SANITIZER_TEST_UBSAN)
    sanitizer_probe_ubsan();
#endif
#if defined(KERNEL_SANITIZER_TEST_KASAN)
    sanitizer_probe_kasan();
#endif
}
