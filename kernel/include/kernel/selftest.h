#ifndef _KERNEL_SELFTEST_H
#define _KERNEL_SELFTEST_H

#include <stdint.h>

// ── Selftest framework ─────────────────────────────────────
// Simple pass/fail test registry for kernel built-in tests.
// Tests run at boot when OS01_SELFTEST=1 is set.
//
// A test returns 0 on PASS, nonzero on FAIL.  Tests should be
// fast (sub-ms) and not depend on external hardware (disk, etc.).
// Tests that need to block or wait should be placed in
// user-space test programs instead.

typedef int (*selftest_fn)(void);

typedef struct {
    const char  *name;
    selftest_fn  fn;
} selftest_entry_t;

// Register a test at compile time.
// Usage:  SELFTEST(slab_alloc_free);
//         SELFTEST(vfs_mount_root);
#define SELFTEST(fn_name)                                           \
    static int selftest_##fn_name(void);                            \
    __attribute__((used, section(".selftest_table")))               \
    static const selftest_entry_t _selftest_##fn_name               \
        = { #fn_name, selftest_##fn_name };                        \
    static int selftest_##fn_name(void)

// Run all registered tests.  Returns 0 if all pass, nonzero otherwise.
int selftest_run_all(void);

#endif // _KERNEL_SELFTEST_H
