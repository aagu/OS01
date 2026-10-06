#ifndef _UAPI_FB_TEST_H
#define _UAPI_FB_TEST_H

/*
 * kernel/include/uapi/fb_test.h
 *
 * Test-only control ABI for the QEMU resolution switcher fault-injection
 * surface (spec §8).  This header is published ONLY in FB_RESOLUTION_TEST
 * builds: the production sysroot never installs it, no /dev/fbtest node
 * is registered, and no injection branch is compiled or linked.
 *
 * The device is /dev/fbtest, driven by the guest helper
 * (user/test_resolution.c) and, for the terminal-ENOMEM hook, by
 * terminal_display.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <uapi/fb.h>

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/* ── Test-only ioctl commands (0x4650..0x4657) ──
 * SNAPSHOT is a pure-output exception (it reads no request struct).
 * TERMINAL_STATUS is also a query (it reads the request struct but writes
 * no state).  Every other command consumes a fixed `struct fb_test_req`. */
#define FBIOTEST_SNAPSHOT                0x00004650
#define FBIOTEST_ARM_MISMATCH            0x00004651
#define FBIOTEST_ARM_ROLLBACK_FAILURE    0x00004652
#define FBIOTEST_HOLD_WRITER             0x00004653
#define FBIOTEST_RELEASE_WRITER          0x00004654
#define FBIOTEST_ARM_TERMINAL_ENOMEM     0x00004655
#define FBIOTEST_CONSUME_TERMINAL_ENOMEM 0x00004656
#define FBIOTEST_TERMINAL_STATUS         0x00004657

/* Fixed control request: version must be 1, reserved[] and reserved64
 * must be all zero.  32 bytes.  token identifies an armed register fault
 * and must be non-zero for the ARM commands. */
struct fb_test_req {
    uint32_t version;
    uint32_t target_pid;
    uint32_t reserved[2];
    uint64_t token;
    uint64_t reserved64;
}; /* 32 bytes, alignment 8, token offset 16 */

/* SNAPSHOT response: current state + DISPI registers in index 0..10 order
 * + active writer-lease count.  64 bytes, reserved must be 0. */
struct fb_test_snapshot {
    struct fb_state state;   /* offset 0, 32 bytes */
    uint16_t regs[11];       /* offset 32, DISPI index 0..10 */
    uint16_t reserved;       /* offset 54, must be 0 */
    uint64_t active_writers; /* offset 56 */
}; /* 64 bytes, alignment 8 */

_Static_assert(sizeof(struct fb_test_req) == 32, "fb_test_req must be 32 bytes (ABI)");
_Static_assert(offsetof(struct fb_test_req, version) == 0, "fb_test_req version offset");
_Static_assert(offsetof(struct fb_test_req, target_pid) == 4, "fb_test_req target_pid offset");
_Static_assert(offsetof(struct fb_test_req, reserved) == 8, "fb_test_req reserved offset");
_Static_assert(offsetof(struct fb_test_req, token) == 16, "fb_test_req token offset must be 16");
_Static_assert(offsetof(struct fb_test_req, reserved64) == 24, "fb_test_req reserved64 offset");

_Static_assert(sizeof(struct fb_test_snapshot) == 64, "fb_test_snapshot must be 64 bytes (ABI)");
_Static_assert(offsetof(struct fb_test_snapshot, state) == 0, "snapshot state offset");
_Static_assert(offsetof(struct fb_test_snapshot, regs) == 32, "snapshot regs offset must be 32");
_Static_assert(offsetof(struct fb_test_snapshot, reserved) == 54, "snapshot reserved offset must be 54");
_Static_assert(offsetof(struct fb_test_snapshot, active_writers) == 56, "snapshot active_writers offset must be 56");

#endif /* _UAPI_FB_TEST_H */
