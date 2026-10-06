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
 * added to the kernel source list (and to the sysroot header set) by the
 * FB_RESOLUTION_TEST=1 build profile.
 */

#include <stdint.h>
#include <stdbool.h>

struct devfs_ops;

enum {
    FB_TEST_STEP_APPLY    = 0, /* new-mode readback verification */
    FB_TEST_STEP_ROLLBACK = 1, /* rollback readback verification */
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

extern const struct devfs_ops fb_test_ops;

#else /* !FB_RESOLUTION_TEST */

#define fb_test_init()                              (0)
#define fb_test_reset()                             ((void)0)
#define fb_test_clear_pending()                     ((void)0)
#define fb_test_set_begin(pid)                      ((void)0)
#define fb_test_set_end()                           ((void)0)
#define fb_test_filter_readback(step, index, value) (value)
#define fb_test_consume_terminal_enomem(pid)        (0)

#endif /* FB_RESOLUTION_TEST */

#endif /* _DRIVER_FB_TEST_H */
