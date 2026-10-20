/*
 * test_gfx_input.c — the gfx input-lease UAPI (GUI-2 plan Task 4)
 *
 * Spec (docs/superpowers/specs/2026-10-20-gui-2-input-focus-design.md)
 * §4.1 (UAPI + libgfx wrapper) + §4.2 (route state machine):
 *   - GFX_BIND_INPUT (0x470A) binds the caller's keyboard / mouse file
 *     descriptors to the caller's DISPLAY VIEW.  The lease is exclusive:
 *     every other reader of the device is shut out (EBUSY / POLLERR) and
 *     the keyboard stops leaking into the console TTY.
 *   - The ioctl is INOUT: the whole 32-byte struct must be readable AND
 *     writable, the input result fields must be 0 on entry, and the
 *     kernel writes the lease generation and the initial modifier
 *     snapshot (bit0 = Caps Lock) back into it.
 *   - The errno mapping: EFAULT (bad / read-only user address), EBADF
 *     (bad fd), EINVAL (not readable / wrong device ops / non-zero
 *     flags / reserved / reserved2 / non-zero input result fields),
 *     EACCES (the caller is not the display owner), ESTALE (the view is
 *     stale) and EBUSY (a lease already exists).
 *   - A failed copy-out rolls the binding back with the CAPTURED
 *     generation: an old rollback must never revoke a newer lease of
 *     the same view (plan Review Focus #1).
 *   - GFX_UNBIND_INPUT (0x470B) releases the CALLER'S OWN view's lease;
 *     unbinding when nothing is bound succeeds.
 *   - libgfx mirrors the ioctl: gfx_bind_input() / gfx_unbind_input().
 *
 * Build:
 *   - The REAL kernel/driver/gfx.c (the ioctl), the REAL
 *     kernel/driver/fb_state.c (the owner / lease coordinator) and the
 *     REAL kernel/driver/input_route.c (the lease itself) are
 *     host-compiled and linked into this binary, so the assertions are
 *     made against the production code and not against a re-implementation.
 *   - The test TU provides the host runtime surface those sources need:
 *     the uaccess primitives (with fault injection), the fd table and
 *     the device-ops lookup, plus the fake keyboard / mouse device ops.
 *   - The REAL libgfx/gfx.c is linked through -Wl,--wrap=open,close,
 *     ioctl,malloc,calloc,free,mmap,munmap so the userspace wrapper is
 *     observed exactly as it calls the kernel.
 */

#include "test_framework.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gfx.h>
#include <driver/fb_state.h>
#include <driver/input_route.h>
#include <uapi/gfx.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <core/printk.h>

/* libgfx's public header — the wrapper under test. */
#include <gfx.h>

/* ── The simulated user address space ───────────────────────────
 * One writable page and one read-only page stand in for the user's
 * mappings; every ioctl argument is staged in one of them, exactly as
 * a real caller would.  A request that points anywhere else faults. */
static uint8_t g_user_page[4096] __attribute__((aligned(4096)));
static uint8_t g_ro_page[4096] __attribute__((aligned(4096)));

static bool range_in(uintptr_t addr, uint64_t len, uintptr_t lo, uintptr_t hi)
{
    return addr >= lo && (addr + len) <= hi;
}

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    if (addr == 0) return false;
    if (len == 0) return true;
    uintptr_t a = (uintptr_t)addr;
    if (range_in(a, len, (uintptr_t)g_user_page,
                 (uintptr_t)g_user_page + sizeof(g_user_page)))
        return true;
    if (range_in(a, len, (uintptr_t)g_ro_page,
                 (uintptr_t)g_ro_page + sizeof(g_ro_page)))
        return !writable;   /* the read-only page rejects a write */
    return false;
}

/* The copy-out fault injection: the copy fails AND the hook runs at
 * the fault point, which is where the interleaved revoke / re-bind of
 * the rollback test happens. */
static int    g_copyout_fail;
static int    g_copyin_fail;
static void (*g_copyout_hook)(void);

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_copyout_fail) {
        g_copyout_fail = 0;
        if (g_copyout_hook) g_copyout_hook();
        return -EFAULT;
    }
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_copyin_fail) {
        g_copyin_fail = 0;
        return -EFAULT;
    }
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ── The fake fd table ─────────────────────────────────────────
 * The kernel resolves the caller's fds through files_get_file() and
 * identifies the device through devfs_get_file_ops(); the table below
 * is what those two lookups see. */
#define MAX_TEST_FDS 8
static struct {
    file_t                *file;
    const struct devfs_ops *ops;
} g_fds[MAX_TEST_FDS];

struct file *files_get_file(struct files_struct *fs, int fd)
{
    (void)fs;
    if (fd < 0 || fd >= MAX_TEST_FDS) return NULL;
    file_t *f = g_fds[fd].file;
    /* The real files_get_file PINS the file: the caller owns a
     * reference until it drops it with file_put(). */
    if (f) file_get(f);
    return f;
}

const struct devfs_ops *devfs_get_file_ops(struct file *f)
{
    if (!f) return NULL;
    for (int i = 0; i < MAX_TEST_FDS; i++) {
        if (g_fds[i].file == f) return g_fds[i].ops;
    }
    return NULL;
}

/* The device ops the bind ioctl verifies against.  The real ones live
 * in the x86-only keyboard / mouse drivers; the host fixture supplies
 * its own so the precise-ops check can be exercised. */
const struct devfs_ops keyboard_ops = { 0 };
const struct devfs_ops mouse_ops = { 0 };
static const struct devfs_ops g_foreign_ops = { 0 };

/* The route's mock runtime releases the endpoint through this hook. */
void input_route_test_set_release_hook(void (*hook)(file_t *f));

/* The NORMAL-path sinks of the route's mock runtime: with them the
 * device behaves normally again once the lease is gone. */
extern const input_route_kbd_sink_t   input_route_test_kbd_sink;
extern const input_route_mouse_sink_t input_route_test_mouse_sink;

/* The present path of the gfx device is not under test here, but the
 * production gfx.c references it — the stub keeps the link closed
 * without dragging the whole framebuffer writer chain into the
 * fixture. */
int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y,
                        const void *pixels, uint32_t len)
{
    (void)lease; (void)x; (void)y; (void)pixels; (void)len;
    return 0;
}

/* ── The libgfx --wrap harness ──────────────────────────────────
 * The wrapper's libc calls are intercepted so the test sees exactly
 * what libgfx asks the kernel for. */
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void  __real_free(void *);
extern void *__real_mmap(void *, size_t, int, int, int, off_t);
extern int   __real_munmap(void *, size_t);

static int mock_open_fd = 7;
static int mock_ioctl_bind_rv;
static int mock_ioctl_unbind_rv;
static gfx_bind_input_t mock_ioctl_bind_value;
static gfx_bind_input_t mock_ioctl_bind_sent;

#define MAX_IOCTL_LOG 8
static struct {
    unsigned long cmd;
    void         *arg;
} mock_ioctl_log[MAX_IOCTL_LOG];
static int mock_ioctl_count;

int __wrap_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    return mock_open_fd++;
}

int __wrap_close(int fd)
{
    (void)fd;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    uint64_t raw_arg = va_arg(ap, uint64_t);
    void *arg = (void *)(uintptr_t)raw_arg;
    va_end(ap);

    if (mock_ioctl_count < MAX_IOCTL_LOG) {
        mock_ioctl_log[mock_ioctl_count].cmd = request;
        mock_ioctl_log[mock_ioctl_count].arg = arg;
    }
    mock_ioctl_count++;

    int rv;
    switch ((int)request) {
    case GFX_CREATE_VIEW:
        rv = 0;
        break;
    case GFX_GET_INFO: {
        static gfx_info_t info = { 4, 4, 16, GFX_FORMAT_RGB32 };
        rv = 0;
        if (arg) memcpy(arg, &info, sizeof(info));
        break;
    }
    case GFX_BIND_INPUT:
        rv = mock_ioctl_bind_rv;
        if (arg) {
            /* Keep what the wrapper asked for, then answer with the
             * lease result the kernel would have written. */
            memcpy(&mock_ioctl_bind_sent, arg, sizeof(mock_ioctl_bind_sent));
            if (rv == 0) {
                memcpy(arg, &mock_ioctl_bind_value, sizeof(mock_ioctl_bind_value));
            }
        }
        break;
    case GFX_UNBIND_INPUT:
        rv = mock_ioctl_unbind_rv;
        break;
    default:
        rv = -ENOTTY;
        break;
    }
    if (rv < 0) {
        errno = -rv;
        return -1;
    }
    return rv;
}

void *__wrap_malloc(size_t size) { return __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t size) { return __real_calloc(n, size); }
void  __wrap_free(void *ptr) { __real_free(ptr); }
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{ return __real_mmap(addr, length, prot, flags, fd, offset); }
int   __wrap_munmap(void *addr, size_t length) { return __real_munmap(addr, length); }

/* ── Fixture ──────────────────────────────────────────────────── */

static uint32_t g_fake_vram[1024 * 768] __attribute__((aligned(4096)));

static file_t *g_view_file;      /* the gfx file that owns the view */
static file_t *g_kbd_file;       /* the keyboard route file */
static file_t *g_mouse_file;     /* the mouse route file */
static file_t *g_other_kbd_file; /* a second keyboard file (a non-owner reader) */

#define FD_KBD    3
#define FD_MOUSE  4
#define FD_OTHER  5

/* The route's host file pool is small: every case releases its files
 * again, so a leaked file can never starve the next case. */
static void test_release_hook(file_t *f)
{
    if (!f) return;
    if (f == g_view_file) {
        /* The gfx device's own release hook (it frees the view). */
        gfx_ops.release_file(f);
        return;
    }
    input_route_release_file(f);
}

static void teardown_env(void)
{
    if (g_view_file)       { file_put(g_view_file);       g_view_file = NULL; }
    if (g_kbd_file)        { file_put(g_kbd_file);        g_kbd_file = NULL; }
    if (g_mouse_file)      { file_put(g_mouse_file);      g_mouse_file = NULL; }
    if (g_other_kbd_file)  { file_put(g_other_kbd_file);  g_other_kbd_file = NULL; }
}

static void setup_env(void)
{
    teardown_env();

    fb_resolution_runtime_reset();
    spin_init(&Pos.lock);

    struct fb_info info = {
        .width = 1024,
        .height = 768,
        .stride = 1024 * 4,
        .bpp = 32,
        .format = FB_FORMAT_RGB32,
    };
    fb_bootstrap_state(0xE0000000ULL, sizeof(g_fake_vram), &info);
    fb_publish_initial_mapping(g_fake_vram, sizeof(g_fake_vram));

    input_route_init();
    input_route_test_set_release_hook(test_release_hook);
    input_route_set_normal_sinks(&input_route_test_kbd_sink,
                                 &input_route_test_mouse_sink);

    memset(g_fds, 0, sizeof(g_fds));
    g_copyout_fail = 0;
    g_copyin_fail = 0;
    g_copyout_hook = NULL;

    /* The input devices: real route files, so the route's own endpoint
     * bookkeeping is exercised. */
    g_kbd_file = NULL;
    g_mouse_file = NULL;
    g_other_kbd_file = NULL;
    input_route_open(INPUT_DEVICE_KEYBOARD, &g_kbd_file);
    input_route_open(INPUT_DEVICE_MOUSE, &g_mouse_file);
    input_route_open(INPUT_DEVICE_KEYBOARD, &g_other_kbd_file);
    g_fds[FD_KBD].file = g_kbd_file;
    g_fds[FD_KBD].ops = &keyboard_ops;
    g_fds[FD_MOUSE].file = g_mouse_file;
    g_fds[FD_MOUSE].ops = &mouse_ops;
    g_fds[FD_OTHER].file = g_other_kbd_file;
    g_fds[FD_OTHER].ops = &keyboard_ops;

    /* The display view: a real gfx file with a configured view. */
    g_view_file = NULL;
    if (gfx_ops.open("/dev/gfx0", &g_view_file) == 0) {
        gfx_view_desc_t desc = { 0, 0, 1024, 768 };
        memcpy(g_user_page, &desc, sizeof(desc));
        gfx_ops.ioctl_file(g_view_file, GFX_CREATE_VIEW, g_user_page);
    }
}

/* Make the view the display owner (the bind requires it). */
static void acquire_ownership(void)
{
    gfx_ops.ioctl_file(g_view_file, GFX_ACQUIRE, NULL);
}

/* The display owner identity is the VIEW, not the file that carries it. */
static const void *view_cookie(void)
{
    return g_view_file ? (const void *)g_view_file->dev_private : NULL;
}

/* Stage a request in the user page and issue the bind ioctl. */
static int do_bind(int kbd_fd, int mouse_fd, gfx_bind_input_t *out)
{
    gfx_bind_input_t req;
    memset(&req, 0, sizeof(req));
    req.keyboard_fd = kbd_fd;
    req.mouse_fd = mouse_fd;
    memcpy(g_user_page, &req, sizeof(req));

    int rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    if (out) memcpy(out, g_user_page, sizeof(*out));
    return rc;
}

/* ── The UAPI layout ──────────────────────────────────────────── */

TEST_FUNC(test_gfx_input_uapi_layout)
{
    TEST_SUITE("gfx bind UAPI: layout");
    /* The REAL UAPI header — the struct is not copied into the test. */
    assert_eq(32, sizeof(gfx_bind_input_t));
    assert_eq(0, offsetof(gfx_bind_input_t, keyboard_fd));
    assert_eq(4, offsetof(gfx_bind_input_t, mouse_fd));
    assert_eq(8, offsetof(gfx_bind_input_t, flags));
    assert_eq(12, offsetof(gfx_bind_input_t, reserved));
    assert_eq(16, offsetof(gfx_bind_input_t, generation));
    assert_eq(24, offsetof(gfx_bind_input_t, initial_modifiers));
    assert_eq(28, offsetof(gfx_bind_input_t, reserved2));

    /* The ioctl numbers are frozen and must not collide with the
     * existing gfx commands (0x4701 - 0x4709). */
    assert_eq(0x470A, (int)GFX_BIND_INPUT);
    assert_eq(0x470B, (int)GFX_UNBIND_INPUT);

    /* libgfx's native result struct is 16 bytes. */
    assert_eq(16, sizeof(gfx_input_state_t));
}

/* ── The argument validation ──────────────────────────────────── */

TEST_FUNC(test_bind_input_einval_on_reserved_fields)
{
    TEST_SUITE("gfx bind UAPI: EINVAL on the reserved fields");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t req;
    int rc;

    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    req.flags = 1;
    memcpy(g_user_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    assert_eq(-EINVAL, rc);

    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    req.reserved = 1;
    memcpy(g_user_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    assert_eq(-EINVAL, rc);

    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    req.reserved2 = 1;
    memcpy(g_user_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    assert_eq(-EINVAL, rc);

    /* The input result fields must be 0 on entry. */
    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    req.generation = 7;
    memcpy(g_user_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    assert_eq(-EINVAL, rc);

    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    req.initial_modifiers = 1;
    memcpy(g_user_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_user_page);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_bind_input_efault_on_bad_user_address)
{
    TEST_SUITE("gfx bind UAPI: EFAULT on a bad user address");
    setup_env();
    acquire_ownership();

    /* An address that is not mapped at all. */
    int rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, (void *)0x1234);
    assert_eq(-EFAULT, rc);

    /* A read-only page: the ioctl is INOUT, so the struct must be
     * writable as well as readable. */
    gfx_bind_input_t req;
    memset(&req, 0, sizeof(req));
    req.keyboard_fd = FD_KBD;
    req.mouse_fd = FD_MOUSE;
    memcpy(g_ro_page, &req, sizeof(req));
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, g_ro_page);
    assert_eq(-EFAULT, rc);

    /* A NULL argument. */
    rc = gfx_ops.ioctl_file(g_view_file, GFX_BIND_INPUT, NULL);
    assert_eq(-EFAULT, rc);
}

TEST_FUNC(test_bind_input_ebadf_on_a_bad_fd)
{
    TEST_SUITE("gfx bind UAPI: EBADF on a bad fd");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;
    /* A fd that is not open. */
    assert_eq(-EBADF, do_bind(MAX_TEST_FDS, FD_MOUSE, &out));
    /* A negative fd. */
    assert_eq(-EBADF, do_bind(-5, FD_MOUSE, &out));
    /* A valid keyboard with a bad mouse. */
    assert_eq(-EBADF, do_bind(FD_KBD, MAX_TEST_FDS, &out));
}

TEST_FUNC(test_bind_input_einval_on_a_bad_device)
{
    TEST_SUITE("gfx bind UAPI: EINVAL on a wrong device");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;

    /* The keyboard fd is not readable. */
    g_kbd_file->flags = O_WRONLY;
    assert_eq(-EINVAL, do_bind(FD_KBD, FD_MOUSE, &out));
    g_kbd_file->flags = O_RDONLY;

    /* The keyboard fd belongs to another device. */
    const struct devfs_ops *saved = g_fds[FD_KBD].ops;
    g_fds[FD_KBD].ops = &g_foreign_ops;
    assert_eq(-EINVAL, do_bind(FD_KBD, FD_MOUSE, &out));
    g_fds[FD_KBD].ops = saved;

    /* The mouse fd is not readable. */
    g_mouse_file->flags = O_WRONLY;
    assert_eq(-EINVAL, do_bind(FD_KBD, FD_MOUSE, &out));
    g_mouse_file->flags = O_RDONLY;

    /* The mouse fd belongs to another device. */
    saved = g_fds[FD_MOUSE].ops;
    g_fds[FD_MOUSE].ops = &g_foreign_ops;
    assert_eq(-EINVAL, do_bind(FD_KBD, FD_MOUSE, &out));
    g_fds[FD_MOUSE].ops = saved;
}

/* ── The display-owner / lease contract ───────────────────────── */

TEST_FUNC(test_bind_input_eacces_when_not_the_owner)
{
    TEST_SUITE("gfx bind UAPI: EACCES for a non-owner");
    setup_env();
    /* The view was never acquired: it is not the display owner. */
    gfx_bind_input_t out;
    assert_eq(-EACCES, do_bind(FD_KBD, FD_MOUSE, &out));
}

TEST_FUNC(test_bind_input_estale_on_an_old_view)
{
    TEST_SUITE("gfx bind UAPI: ESTALE for an old view");
    setup_env();
    acquire_ownership();

    /* The display generation moved on: the view's mode sequence is
     * stale, so the bind must be refused. */
    fb_state__test_set_generation(99);

    gfx_bind_input_t out;
    assert_eq(-ESTALE, do_bind(FD_KBD, FD_MOUSE, &out));
}

TEST_FUNC(test_bind_input_ebusy_on_a_duplicate_bind)
{
    TEST_SUITE("gfx bind UAPI: EBUSY for a duplicate bind");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;
    assert_eq(0, do_bind(FD_KBD, FD_MOUSE, &out));
    assert_true(out.generation != 0);
    assert_eq(0, out.reserved2);

    /* A second bind does not preempt the live lease. */
    assert_eq(-EBUSY, do_bind(FD_KBD, FD_MOUSE, &out));
}

TEST_FUNC(test_bind_input_grants_an_exclusive_lease)
{
    TEST_SUITE("gfx bind UAPI: the lease is exclusive");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;
    assert_eq(0, do_bind(FD_KBD, FD_MOUSE, &out));
    assert_true(out.generation != 0);

    /* The owner's file reads the ACTIVE queue ... */
    input_route_keyboard_byte(0x1E);
    uint8_t buf[8];
    assert_eq(1, input_route_read(g_kbd_file, 0, sizeof(buf), buf));
    assert_eq(0x1E, buf[0]);

    /* ... and every other reader is shut out. */
    assert_eq(-EBUSY, input_route_read(g_other_kbd_file, 0, sizeof(buf), buf));

    /* An explicit unbind releases the lease: the device is back on the
     * NORMAL path and every reader may read again. */
    assert_eq(0, gfx_ops.ioctl_file(g_view_file, GFX_UNBIND_INPUT, NULL));
    input_route_keyboard_byte(0x1E);
    assert_eq(1, input_route_read(g_other_kbd_file, 0, sizeof(buf), buf));
    assert_eq(0x1E, buf[0]);
}

TEST_FUNC(test_bind_input_keyboard_only_lease)
{
    TEST_SUITE("gfx bind UAPI: the keyboard-only lease");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;
    /* mouse_fd = -1: the keyboard alone is leased. */
    assert_eq(0, do_bind(FD_KBD, -1, &out));
    assert_true(out.generation != 0);

    uint8_t buf[16];
    /* The keyboard is exclusive ... */
    input_route_keyboard_byte(0x1E);
    assert_eq(1, input_route_read(g_kbd_file, 0, sizeof(buf), buf));
    assert_eq(0x1E, buf[0]);
    assert_eq(-EBUSY, input_route_read(g_other_kbd_file, 0, sizeof(buf), buf));

    /* ... and the mouse is not leased: it stays on the NORMAL path. */
    mouse_event_t ev = { .buttons = 1, .wheel = 0, .dx = 3, .dy = 0 };
    input_route_mouse_event(&ev);
    assert_eq((long)sizeof(mouse_event_t),
              (long)input_route_read(g_mouse_file, 0, sizeof(buf), buf));
}

TEST_FUNC(test_unbind_input_succeeds_without_a_lease)
{
    TEST_SUITE("gfx bind UAPI: unbind without a lease");
    setup_env();
    acquire_ownership();

    /* Nothing is bound: the unbind still succeeds. */
    assert_eq(0, gfx_ops.ioctl_file(g_view_file, GFX_UNBIND_INPUT, NULL));

    /* And after a bind / unbind cycle the lease is gone. */
    gfx_bind_input_t out;
    assert_eq(0, do_bind(FD_KBD, FD_MOUSE, &out));
    assert_eq(0, gfx_ops.ioctl_file(g_view_file, GFX_UNBIND_INPUT, NULL));
    assert_eq(0, do_bind(FD_KBD, FD_MOUSE, &out));
}

TEST_FUNC(test_bind_input_copyout_failure_rolls_the_lease_back)
{
    TEST_SUITE("gfx bind UAPI: the copy-out failure rolls the lease back");
    setup_env();
    acquire_ownership();

    gfx_bind_input_t out;
    g_copyout_fail = 1;
    assert_eq(-EFAULT, do_bind(FD_KBD, FD_MOUSE, &out));

    /* The lease was rolled back: the device is back on the NORMAL path
     * (every reader may read again) and a new bind is possible. */
    uint8_t buf[8];
    input_route_keyboard_byte(0x1E);
    assert_eq(1, input_route_read(g_other_kbd_file, 0, sizeof(buf), buf));
    assert_eq(0, do_bind(FD_KBD, FD_MOUSE, &out));
}

/* The plan's Review Focus #1: an old rollback must never revoke a
 * newer lease of the same view. */
static const void *g_hook_view;
static uint64_t g_hook_generation;

static void hook_revoke_and_rebind(void)
{
    uint64_t generation = 0;
    uint32_t modifiers = 0;
    const void *view = g_hook_view;

    /* The lease is released (an explicit cleanup) and the view binds
     * again: a NEW generation. */
    fb_input_unbind(view, 0, 0);
    assert_eq(0, fb_input_bind(view, 0, g_kbd_file, g_mouse_file,
                               &generation, &modifiers));
    assert_true(generation != 0);
    g_hook_generation = generation;
}

TEST_FUNC(test_copyout_rollback_does_not_revoke_new_generation)
{
    TEST_SUITE("gfx bind UAPI: an old rollback keeps the new lease");
    setup_env();
    acquire_ownership();

    /* The bind's copy-out fails.  At the fault point — with every lock
     * dropped — the lease is released and bound again, so the rollback
     * that follows belongs to the OLD generation. */
    g_hook_view = view_cookie();
    g_copyout_hook = hook_revoke_and_rebind;
    g_copyout_fail = 1;

    gfx_bind_input_t out;
    assert_eq(-EFAULT, do_bind(FD_KBD, FD_MOUSE, &out));
    g_copyout_hook = NULL;

    /* The new lease is still the live one: the stale rollback did not
     * take it away. */
    assert_true(g_hook_generation != 0);

    uint8_t buf[8];
    input_route_keyboard_byte(0x1E);
    assert_eq(1, input_route_read(g_kbd_file, 0, sizeof(buf), buf));
    assert_eq(0x1E, buf[0]);

    /* The other reader is still shut out: the lease is really alive. */
    assert_eq(-EBUSY, input_route_read(g_other_kbd_file, 0, sizeof(buf), buf));
}

/* ── The libgfx wrapper ───────────────────────────────────────── */

static void reset_libgfx_mocks(void)
{
    mock_open_fd = 7;
    mock_ioctl_count = 0;
    mock_ioctl_bind_rv = 0;
    mock_ioctl_unbind_rv = 0;
    memset(&mock_ioctl_bind_value, 0, sizeof(mock_ioctl_bind_value));
    memset(&mock_ioctl_bind_sent, 0, sizeof(mock_ioctl_bind_sent));
    memset(mock_ioctl_log, 0, sizeof(mock_ioctl_log));
}

TEST_FUNC(test_libgfx_bind_input_success)
{
    TEST_SUITE("libgfx: bind input");
    reset_libgfx_mocks();

    mock_ioctl_bind_value.generation = 42;
    mock_ioctl_bind_value.initial_modifiers = 1;

    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);

    gfx_input_state_t state;
    memset(&state, 0, sizeof(state));
    errno = 0;
    int rc = gfx_bind_input(h, 5, 6, &state);
    assert_eq(0, rc);
    assert_eq(42, (long)state.generation);
    assert_eq(1, (long)state.initial_modifiers);
    assert_eq(0, (long)state.reserved);

    /* open -> CREATE_VIEW -> GET_INFO -> BIND */
    assert_eq(3, mock_ioctl_count);
    assert_eq((int)GFX_BIND_INPUT, (int)mock_ioctl_log[2].cmd);
    assert_eq(5, (long)mock_ioctl_bind_sent.keyboard_fd);
    assert_eq(6, (long)mock_ioctl_bind_sent.mouse_fd);
    assert_eq(0, (long)mock_ioctl_bind_sent.flags);
    assert_eq(0, (long)mock_ioctl_bind_sent.reserved);
    assert_eq(0, (long)mock_ioctl_bind_sent.reserved2);

    gfx_close(h);
}

TEST_FUNC(test_libgfx_bind_input_null_arguments)
{
    TEST_SUITE("libgfx: bind input argument checks");
    reset_libgfx_mocks();

    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);
    int ioctls_after_open = mock_ioctl_count;

    gfx_input_state_t state;
    errno = 0;
    assert_eq(-1, gfx_bind_input(NULL, 5, 6, &state));
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    errno = 0;
    assert_eq(-1, gfx_bind_input(h, 5, 6, NULL));
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    errno = 0;
    assert_eq(-1, gfx_unbind_input(NULL));
    assert_eq(EINVAL, errno);
    assert_eq(ioctls_after_open, mock_ioctl_count);

    gfx_close(h);
}

TEST_FUNC(test_libgfx_bind_input_errno_passthrough)
{
    TEST_SUITE("libgfx: bind input errno passthrough");
    reset_libgfx_mocks();

    gfx_handle_t *h = gfx_open(0, 0, 4, 4);
    assert_not_null(h);

    const int errs[] = { EACCES, ESTALE, EBUSY, EINVAL, EFAULT, EBADF };
    for (int i = 0; i < 6; i++) {
        mock_ioctl_bind_rv = -errs[i];
        gfx_input_state_t state;
        memset(&state, 0, sizeof(state));
        errno = 0;
        assert_eq(-1, gfx_bind_input(h, 5, 6, &state));
        assert_eq(errs[i], errno);
    }

    mock_ioctl_bind_rv = 0;
    mock_ioctl_unbind_rv = -EIO;
    errno = 0;
    assert_eq(-1, gfx_unbind_input(h));
    assert_eq(EIO, errno);

    gfx_close(h);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_gfx_input_uapi_layout),
    TEST_ENTRY(test_bind_input_einval_on_reserved_fields),
    TEST_ENTRY(test_bind_input_efault_on_bad_user_address),
    TEST_ENTRY(test_bind_input_ebadf_on_a_bad_fd),
    TEST_ENTRY(test_bind_input_einval_on_a_bad_device),
    TEST_ENTRY(test_bind_input_eacces_when_not_the_owner),
    TEST_ENTRY(test_bind_input_estale_on_an_old_view),
    TEST_ENTRY(test_bind_input_ebusy_on_a_duplicate_bind),
    TEST_ENTRY(test_bind_input_grants_an_exclusive_lease),
    TEST_ENTRY(test_bind_input_keyboard_only_lease),
    TEST_ENTRY(test_unbind_input_succeeds_without_a_lease),
    TEST_ENTRY(test_bind_input_copyout_failure_rolls_the_lease_back),
    TEST_ENTRY(test_copyout_rollback_does_not_revoke_new_generation),
    TEST_ENTRY(test_libgfx_bind_input_success),
    TEST_ENTRY(test_libgfx_bind_input_null_arguments),
    TEST_ENTRY(test_libgfx_bind_input_errno_passthrough),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
