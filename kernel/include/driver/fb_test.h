#ifndef _DRIVER_FB_TEST_H
#define _DRIVER_FB_TEST_H

/*
 * kernel/include/driver/fb_test.h
 *
 * Private test-only entry points for the framebuffer fault-injection
 * surface (Task 9).  Production TUs (fb_state.c / bga.c / fb.c) include
 * this header unconditionally but compile with FB_RESOLUTION_TEST
 * undefined, so every call site below expands to a no-op: no injection
 * branch is compiled and no fb_test.c symbol is linked in.
 *
 * The real implementations live in kernel/driver/fb_test.c, which is only
 * added to the kernel source list by the FB_RESOLUTION_TEST=1 build profile.
 * This private header is never published to the userspace sysroot; test
 * clients use the control ABI in <uapi/fb_test.h>.
 */

#include <stdint.h>
#include <stdbool.h>

struct devfs_ops;

enum {
    FB_TEST_STEP_APPLY    = 0, /* new-mode readback verification */
    FB_TEST_STEP_ROLLBACK = 1, /* rollback readback verification */
};

/* The fault classes that carry a token. */
enum fb_test_fault_class {
    FB_TEST_FAULT_REGISTER = 0, /* ARM_MISMATCH / ARM_ROLLBACK_FAILURE */
    FB_TEST_FAULT_TERMINAL = 1, /* ARM_TERMINAL_ENOMEM */
};

#ifdef FB_RESOLUTION_TEST

/* Register /dev/fbtest.  Idempotent; returns 0 once registered. */
int fb_test_init(void);

/* Clear every pending (armed) fault record.  Called at bootstrap and when
 * the backend is permanently failed. */
void fb_test_reset(void);
void fb_test_clear_pending(void);

/* SET-transaction hooks (fb_state.c).  begin() consumes at most one armed
 * register-readback fault whose target PID matches; end() drops the
 * per-transaction state. */
void fb_test_set_begin(int32_t pid);
void fb_test_set_end(void);

/* Readback filter (bga.c).  Returns `value` unchanged unless the active
 * transaction carries a one-shot fault for this step. */
uint16_t fb_test_filter_readback(int step, uint16_t index, uint16_t value);

/* Terminal-ENOMEM consumer (terminal_display.c).  Returns 1 (once) when a
 * prepare fault is armed for `pid`, else 0. */
int fb_test_consume_terminal_enomem(int32_t pid);

/* The token of the most recently CONSUMED fault of the given class, or 0
 * when no fault of that class has been consumed yet.
 *
 * The token is the DISCRIMINATOR that proves WHICH armed fault fired.
 * Without it a test can only count consumptions and cannot tell "the
 * fault I armed was taken" from "a leftover fault from an earlier case
 * was taken" — the token was stored but never read, so it discriminated
 * nothing.  Roadmap parked item "QEMU 分辨率切换器收尾 follow-up" #4.
 *
 * It is deliberately NOT part of the test-only UAPI: the /dev/fbtest
 * ioctl contract stays frozen, and the token is only needed by the host
 * test, which links this kernel-private entry point directly.  The
 * production sysroot never installs this header. */
uint64_t fb_test_last_consumed_token(enum fb_test_fault_class cls);

extern const struct devfs_ops fb_test_ops;

#else /* !FB_RESOLUTION_TEST */

#define fb_test_init()                              (0)
#define fb_test_reset()                             ((void)0)
#define fb_test_clear_pending()                     ((void)0)
#define fb_test_set_begin(pid)                      ((void)0)
#define fb_test_set_end()                           ((void)0)
#define fb_test_filter_readback(step, index, value) (value)
#define fb_test_consume_terminal_enomem(pid)        (0)
#define fb_test_last_consumed_token(cls)            (0ull)

#endif /* FB_RESOLUTION_TEST */

#endif /* _DRIVER_FB_TEST_H */
