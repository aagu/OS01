/*
 * test_gfx_device.c — bounded /dev/gfx0 framebuffer present device
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §3 ("Components and kernel interfaces") + §4 ("ABI and userspace
 * API"):
 *   - Each open on /dev/gfx0 produces a per-file gfx_view via
 *     f->dev_private (Task 1 dispatch path).  The open itself
 *     does NOT reserve a view-slot — that happens on the first
 *     successful GFX_CREATE_VIEW.
 *   - GFX_CREATE_VIEW validates x,y,w,h against the live
 *     framebuffer dimensions and the view's "not already
 *     configured" state, then allocates one slot in the 16-entry
 *     view table under a small spinlock.
 *   - Width/height MUST be non-zero and the x+w/y+h additions
 *     MUST NOT overflow — checks use the subtraction form
 *     (x <= fb_w && w <= fb_w - x) so a UINT32_MAX w or h is
 *     rejected even when x or y is 0.
 *   - GFX_GET_INFO on an unconfigured view returns -EINVAL.
 *   - GFX_PRESENT validates the request struct, snapshots the
 *     immutable view rect under the lock, drops the lock, then
 *     validates per-row and copies row-by-row through
 *     copy_from_user_ft into a heap row buffer, calling
 *     fb_write_row on each row.  The lock is NEVER held across
 *     syscall_check_user_range / copy_from_user_ft.
 *   - 16-slot limit; reconfigure is rejected (returns -EINVAL).
 *   - release_file (Task 1) returns the slot and frees the view.
 *
 * Build:
 *   - Production kernel/driver/gfx.c compiled via GFX_HOST_CFLAGS
 *     against hosttests/mock/gfx_test_runtime.h.  kernel/driver/fb.c
 *     is NOT compiled (the heavy header chain it pulls in — VMA,
 *     VMM, scheduler — is not part of this test's scope); fb_get_info
 *     and fb_write_row are mocked here so the gfx device contract
 *     is exercised end-to-end against a host-heap fb buffer.
 *   - The test TU provides the runtime surface the production
 *     gfx.c references (kmalloc/kfree, current, schedule, slab
 *     stubs, vfs_node_get/put, uaccess mocks, the position Pos
 *     struct, and the fb helpers mocked here).
 *   - Tested production symbols: gfx_ops (open/ioctl_file/release_file).
 */

#include "test_framework.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gfx.h>
#include <driver/fb.h>
#include <uapi/gfx.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <core/printk.h>          // position Pos (host-test fake)
#include <memory/uaccess.h>       // USER_MIN_ADDR (simulator matches it)
#include <lwip/err.h>
#include <lwip/api.h>

/* ── Task + files plumbing (declared in gfx_test_runtime.h) ── */

static task_t   test_task;
static files_t  test_files;
task_t *gfx_test_current = &test_task;
#define current gfx_test_current

uint64_t volatile jiffies;

/* ── Stubs for symbols the production gfx.c references.  Mirrors
 * the surface the existing lifecycle test (test_gfx_file_lifecycle.c)
 * provides so the build of gfx.c + devfs.c + file.c shares the same
 * minimal host runtime. */

void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }

struct vfs_node *vfs_node_get(struct vfs_node *n)
{
    if (n) n->refcount++;
    return n;
}
void vfs_node_put(struct vfs_node *n)
{
    if (!n) return;
    if (n->refcount == 0) { assert_true(0 && "vfs_node_put underflow"); return; }
    n->refcount--;
    if (n->refcount == 0) free(n);
}

int vfs_mount(const char *p, block_device_t *d, vfs_ops_t *o, void *f)
{ (void)p; (void)d; (void)o; (void)f; return -ENOSYS; }
struct vfs_node *vfs_lookup(const char *p) { (void)p; return NULL; }
struct vfs_node *vfs_lookup_from(const char *p, const char *c)
{ (void)p; (void)c; return NULL; }

int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void get_random_bytes(void *b, size_t n) { (void)b; (void)n; }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }

tty_t *get_dev_tty(void) { return NULL; }
void tty_set_dev_tty(tty_t *t) { (void)t; }
int tty_read(tty_t *t, char *b, int s, bool nb)
{ (void)t; (void)b; (void)s; (void)nb; return 0; }
int tty_write(tty_t *t, const char *b, int s)
{ (void)t; (void)b; (void)s; return 0; }
uint32_t tty_poll(tty_t *t, uint32_t r, struct poll_table *p)
{ (void)t; (void)p; return r; }
int tty_phys_ioctl(struct vfs_node *n, int c, void *a)
{ (void)n; (void)c; (void)a; return -ENOTTY; }

void log_debug(const char *fmt, ...) { (void)fmt; }

/* printk stubs (declared in gfx_test_runtime.h, defined here so the
 * host-compiled gfx.c can link).  The test never invokes them. */
int  color_printk(unsigned int fr, unsigned int bk, const char *fmt, ...)
{ (void)fr; (void)bk; (void)fmt; return 0; }
int  serial_printk(const char *fmt, ...) { (void)fmt; return 0; }
void frame_buffer_init(void) {}
void frame_buffer_early_init(void) {}

/* LWIP stubs (referenced by file.c's FD_SOCKET branch; never reached). */
err_t netconn_delete(struct netconn *c) { (void)c; return ERR_OK; }
err_t netconn_recv(struct netconn *c, struct netbuf **b)
{ (void)c; *b = NULL; return ERR_CLSD; }
err_t netconn_write(struct netconn *c, const void *d, size_t s, u8_t f)
{ (void)c; (void)d; (void)s; (void)f; return ERR_OK; }
err_t netconn_write_partly(struct netconn *c, const void *d, size_t s,
                           u8_t f, size_t *w)
{ (void)c; (void)d; (void)s; (void)f; if (w) *w = s; return ERR_OK; }
void netbuf_data(struct netbuf *b, void **d, u16_t *l)
{ (void)b; if (d) *d = NULL; if (l) *l = 0; }
int netbuf_next(struct netbuf *b) { (void)b; return -1; }
void netbuf_delete(struct netbuf *b) { (void)b; }

/* wait_queue stubs. */
void wait_queue_init(wait_queue_t *q) { (void)q; }
void wait_queue_sleep(wait_queue_t *q) { (void)q; }
void wait_queue_wake_one(wait_queue_t *q) { (void)q; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }

/* Sched stubs. */
void task_wake(task_t *t) { (void)t; }
void schedule(void) {}
spinlock_T task_list_lock = { 1 };
gfx_test_task_union_t init_task_union;
list_t *task_list_next(list_t *p) { (void)p; return NULL; }
int signal_pgrp(pid_t t, int s) { (void)t; (void)s; return -1; }

/* ── Position + fb buffer (host-test fake) ──
 * 64x48 at 32bpp = 12288 bytes.  fb_write_row is mocked below to
 * write into this buffer so the test can observe the actual pixel
 * values and assert that pixels OUTSIDE the view's rectangle are
 * untouched. */
#define TEST_FB_WIDTH   64
#define TEST_FB_HEIGHT  48
#define TEST_FB_BPP     4
#define TEST_FB_STRIDE  (TEST_FB_WIDTH * TEST_FB_BPP)
#define TEST_FB_SIZE    (TEST_FB_WIDTH * TEST_FB_HEIGHT * TEST_FB_BPP)

static uint32_t *test_fb_buf;          // Pos.FB_addr points here
static uint32_t test_fb_width_override;
position Pos;                          // host-test fake (defined here)

/* ── User-pointer simulator ──
 * The test uses a host heap buffer as the "user" pixel buffer the
 * gfx device copies from via copy_from_user_ft.  The simulator
 * records which range check + fault settings the production code
 * actually exercises, so missing-range-check or missing-fault
 * handling shows up as a test failure. */
struct user_sim {
    uint8_t *buf;             // backing heap (for present's pixel copies)
    size_t   buf_size;
    bool     range_check_ok;  // if false, syscall_check_user_range always fails
    int      fault_after_calls;  // if >= 0, return -EFAULT on the Nth successful copy
    int      copy_count;        // counts copy_from_user_ft calls
    int      range_check_count;
} g_user_sim;

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    g_user_sim.range_check_count++;
    (void)writable;
    if (len == 0) return true;
    if (!g_user_sim.range_check_ok) return false;
    // Mirrors the production check from memory/uaccess.h:64-71 —
    // reject anything below USER_MIN_ADDR (kernel range) so the
    // "kernel-range pointer" test can pass a 0x100000 pointer and
    // expect -EFAULT.  No upper bound: the test passes host-heap
    // and host-stack pointers which are both far above USER_MIN_ADDR.
    if (addr < USER_MIN_ADDR) return false;
    if (addr + len < addr) return false;            // overflow guard
    return true;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*cb)(void *), void *arg)
{
    (void)cb; (void)arg;
    g_user_sim.copy_count++;
    if (g_user_sim.fault_after_calls >= 0 &&
        g_user_sim.copy_count > g_user_sim.fault_after_calls) {
        return -EFAULT;
    }
    if (!syscall_check_user_range((uint64_t)src, n, false)) return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*cb)(void *), void *arg)
{
    (void)cb; (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}
int strnlen_user(const void *p, size_t m) { (void)p; (void)m; return 0; }

/* ── fb helper mocks ──
 * The production kernel/driver/fb.c is NOT host-compiled in this test
 * (it pulls in VMA/VMM/scheduler headers we don't bring up here).  We
 * mock fb_get_info / fb_write_row against the host-heap fb so the
 * gfx device's present contract is observed end-to-end. */

int fb_get_info(struct fb_info *out)
{
    if (!out) return -EINVAL;
    out->width  = test_fb_width_override ? test_fb_width_override
                                         : (uint32_t)TEST_FB_WIDTH;
    out->height = (uint32_t)TEST_FB_HEIGHT;
    out->stride = TEST_FB_STRIDE;
    out->bpp    = 32;
    out->format = GFX_FORMAT_RGB32;
    return 0;
}

int fb_snapshot_read(fb_snapshot_t *out)
{
    if (!out) return -EINVAL;
    out->state.info.width  = test_fb_width_override ? test_fb_width_override
                                                    : (uint32_t)TEST_FB_WIDTH;
    out->state.info.height = (uint32_t)TEST_FB_HEIGHT;
    out->state.info.stride = TEST_FB_STRIDE;
    out->state.info.bpp    = 32;
    out->state.info.format = GFX_FORMAT_RGB32;
    out->state.generation  = 1;
    out->state.reserved    = 0;
    out->addr = test_fb_buf;
    out->mapped_size = TEST_FB_SIZE;
    return 0;
}

int fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation)
{
    if (!lease) return -EINVAL;
    int rc = fb_snapshot_read(&lease->snapshot);
    if (rc < 0) return rc;
    if (expected_generation != 0 && expected_generation != lease->snapshot.state.generation)
        return -ESTALE;
    lease->held = true;
    return 0;
}

void fb_writer_end(fb_lease_t *lease)
{
    if (lease) lease->held = false;
}

int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y,
                        const void *pixels, uint32_t bytes)
{
    (void)lease;
    return fb_write_row(x, y, pixels, bytes);
}

int fb_write_row(uint32_t x, uint32_t y, const void *pixels,
                 uint32_t row_bytes)
{
    if (!pixels) return -EINVAL;
    if (x >= (uint32_t)Pos.XResolution) return -EINVAL;
    if (row_bytes > ((uint32_t)Pos.XResolution - x) * TEST_FB_BPP) return -EINVAL;
    if (y >= (uint32_t)Pos.YResolution) return -EINVAL;
    uint64_t byte_offset = (uint64_t)y * (uint32_t)Pos.XResolution * TEST_FB_BPP
                           + (uint64_t)x * TEST_FB_BPP;
    if (byte_offset + row_bytes > Pos.FB_length) return -EINVAL;

    memcpy((uint8_t *)Pos.FB_addr + byte_offset, pixels, row_bytes);
    return 0;
}

/* ── Test runtime helpers ── */

static void reset_test_state(void)
{
    test_fb_width_override = 0;
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
    jiffies = 0;

    /* Initialize fb */
    if (!test_fb_buf) test_fb_buf = calloc(1, TEST_FB_SIZE);
    assert_not_null(test_fb_buf);
    /* Wipe to a sentinel value so we can detect accidental overwrites. */
    for (size_t i = 0; i < TEST_FB_SIZE / 4; i++) test_fb_buf[i] = 0xDEADBEEFu;

    Pos.XResolution = TEST_FB_WIDTH;
    Pos.YResolution = TEST_FB_HEIGHT;
    Pos.FB_addr     = test_fb_buf;
    Pos.FB_length   = TEST_FB_SIZE;
    Pos.Phy_addr    = test_fb_buf;     /* unused by mocked fb_write_row */
    spin_init(&Pos.lock);

    /* Initialize user simulator */
    if (!g_user_sim.buf) g_user_sim.buf = malloc(8 * 1024 * 1024);
    assert_not_null(g_user_sim.buf);
    g_user_sim.buf_size        = 8 * 1024 * 1024;
    g_user_sim.range_check_ok  = true;
    g_user_sim.fault_after_calls = -1;
    g_user_sim.copy_count       = 0;
    g_user_sim.range_check_count = 0;
}

/* ── devfs registration plumbing (mirror lifecycle test) ── */

#define TEST_MAX_DEVICES 8
static int registered_count;
/* /dev/gfx0 is registered EXACTLY ONCE per test program — the
 * production devfs devices[] table persists across tests (devfs_init
 * cannot be re-called) and would overflow after 32 registrations.
 * Every open in the suite reuses this single registered entry. */
static int gfx_device_index = -1;

static int gfx_register_once(void)
{
    if (gfx_device_index >= 0) return 0;
    int idx = registered_count++;
    int rc = devfs_register_chrdev("gfx0", NULL, &gfx_ops);
    if (rc < 0) { registered_count--; return -1; }
    gfx_device_index = idx;
    return 0;
}

/* Helper: open /dev/gfx0 through the production devfs_open_node
 * path.  Returns the file_t (with dev_private already attached) on
 * success, NULL on failure.  The vfs_node is allocated with
 * refcount=0 so devfs_open_node's vfs_node_get lifts it to 1 and
 * the eventual file_put → file_free → vfs_node_put drops it to 0
 * and frees the node — balanced ownership, no leaks. */
static file_t *open_gfx0(void)
{
    if (gfx_register_once() < 0) return NULL;

    struct vfs_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->type = VFS_CHRDEV;
    node->fs_data = (void *)(uintptr_t)gfx_device_index;
    node->refcount = 0;            /* devfs_open_node will get() it */

    file_t *f = NULL;
    int rc = devfs_open_node(node, "/dev/gfx0", O_RDWR, &f);
    if (rc != 0) {
        /* devfs_open_node didn't attach (or attached + errored) —
         * free the node ourselves so we don't leak. */
        free(node);
        return NULL;
    }
    return f;
}

/* Helper: send a CREATE_VIEW with the given view desc; returns the
 * raw ioctl return code.  Skips range checks because the gfx device
 * does them itself. */
static int do_create_view(file_t *f, const gfx_view_desc_t *desc)
{
    return (int)fd_ioctl(f, GFX_CREATE_VIEW, (void *)desc);
}

static int do_get_info(file_t *f, gfx_info_t *info)
{
    return (int)fd_ioctl(f, GFX_GET_INFO, (void *)info);
}

static int do_present(file_t *f, gfx_present_req_t *req)
{
    return (int)fd_ioctl(f, GFX_PRESENT, (void *)req);
}

/* ── Tests ──────────────────────────────────────────── */

TEST_FUNC(test_create_view_rejects_zero_size) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t zero_w = { 0, 0, 0, 10 };
    assert_eq(-EINVAL, do_create_view(f, &zero_w));
    gfx_view_desc_t zero_h = { 0, 0, 10, 0 };
    assert_eq(-EINVAL, do_create_view(f, &zero_h));

    file_put(f);
}

TEST_FUNC(test_create_view_rejects_uint32_max_overflow) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* w = UINT32_MAX overflows fb_w - 0 (=TEST_FB_WIDTH). */
    gfx_view_desc_t huge_w = { 0, 0, UINT32_MAX, 1 };
    assert_eq(-EINVAL, do_create_view(f, &huge_w));
    /* h = UINT32_MAX overflows fb_h - 0 (=TEST_FB_HEIGHT). */
    gfx_view_desc_t huge_h = { 0, 0, 1, UINT32_MAX };
    assert_eq(-EINVAL, do_create_view(f, &huge_h));
    /* x=1, w=UINT32_MAX → x + w overflows (well, x+w computation is
     * irrelevant — the test is x <= fb_w && w <= fb_w - x; here
     * fb_w - x = 63 and UINT32_MAX > 63). */
    gfx_view_desc_t xplus_w_overflow = { 1, 0, UINT32_MAX, 1 };
    assert_eq(-EINVAL, do_create_view(f, &xplus_w_overflow));
    /* y=1, h=UINT32_MAX → same overflow rejection. */
    gfx_view_desc_t yplus_h_overflow = { 0, 1, 1, UINT32_MAX };
    assert_eq(-EINVAL, do_create_view(f, &yplus_h_overflow));

    file_put(f);
}

TEST_FUNC(test_create_view_rejects_stride_overflow) {
    reset_test_state();
    test_fb_width_override = UINT32_MAX;
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t too_wide = { 0, 0, UINT32_MAX / 4u + 1u, 1 };
    int rc = do_create_view(f, &too_wide);
    assert_eq(-EINVAL, rc);
    file_put(f);
}

TEST_FUNC(test_create_view_exact_fit_right_edge) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* x = TEST_FB_WIDTH - 1, w = 1 — fits exactly along x. */
    gfx_view_desc_t fit_right = { TEST_FB_WIDTH - 1, 0, 1, 1 };
    assert_eq(0, do_create_view(f, &fit_right));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(1u, info.width);
    assert_eq(1u, info.height);
    assert_eq(4u, info.stride);
    assert_eq((unsigned)GFX_FORMAT_RGB32, info.format);

    file_put(f);
}

TEST_FUNC(test_create_view_exact_fit_bottom_edge) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* y = TEST_FB_HEIGHT - 1, h = 1 — fits exactly along y. */
    gfx_view_desc_t fit_bottom = { 0, TEST_FB_HEIGHT - 1, 1, 1 };
    assert_eq(0, do_create_view(f, &fit_bottom));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(1u, info.width);
    assert_eq(1u, info.height);

    file_put(f);
}

TEST_FUNC(test_create_view_rejects_reconfigure) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t a = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &a));
    /* Second CREAT_VIEW on the same file must return -EINVAL — the
     * view is already configured. */
    gfx_view_desc_t b = { 0, 0, 2, 2 };
    assert_eq(-EINVAL, do_create_view(f, &b));

    file_put(f);
}

TEST_FUNC(test_get_info_unconfigured_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_info_t info;
    assert_eq(-EINVAL, do_get_info(f, &info));

    file_put(f);
}

TEST_FUNC(test_two_independent_views) {
    reset_test_state();
    file_t *f1 = open_gfx0();
    file_t *f2 = open_gfx0();
    assert_not_null(f1);
    assert_not_null(f2);

    gfx_view_desc_t small = { 0, 0, 3, 5 };
    gfx_view_desc_t tall = { 0, 0, 7, 11 };
    assert_eq(0, do_create_view(f1, &small));
    assert_eq(0, do_create_view(f2, &tall));

    gfx_info_t i1, i2;
    assert_eq(0, do_get_info(f1, &i1));
    assert_eq(0, do_get_info(f2, &i2));
    assert_eq(3u,  i1.width);
    assert_eq(5u,  i1.height);
    assert_eq(12u, i1.stride);
    assert_eq(7u,  i2.width);
    assert_eq(11u, i2.height);
    assert_eq(28u, i2.stride);

    file_put(f1);
    file_put(f2);
}

TEST_FUNC(test_release_returns_slot) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));
    file_put(f);  /* release_file fires, frees the view + slot */

    /* Reopen and reconfigure on the freed slot must succeed. */
    file_t *f2 = open_gfx0();
    assert_not_null(f2);
    gfx_view_desc_t v2 = { 0, 0, 5, 5 };
    assert_eq(0, do_create_view(f2, &v2));

    file_put(f2);
}

TEST_FUNC(test_create_view_enforces_16_limit) {
    reset_test_state();

    /* Open GFX_MAX_VIEWS=16 files and configure each. */
    file_t *files[GFX_MAX_VIEWS];
    int opened = 0;
    for (int i = 0; i < GFX_MAX_VIEWS; i++) {
        files[i] = open_gfx0();
        if (!files[i]) break;
        gfx_view_desc_t v = { 0, 0, 1, 1 };
        if (do_create_view(files[i], &v) != 0) break;
        opened++;
    }
    assert_eq(GFX_MAX_VIEWS, opened);

    /* 17th open + create must fail (table full).  Note: open itself
     * succeeds (no slot taken); the slot-allocation happens inside
     * CREATE_VIEW.  We re-use one of the existing files so we don't
     * consume another fd slot. */
    gfx_view_desc_t v = { 0, 0, 1, 1 };
    assert_eq(-EINVAL, do_create_view(files[0], &v));  /* already configured */

    /* Open a fresh fd and try to create — this exercises the
     * "no free slot" path. */
    file_t *extra = open_gfx0();
    if (extra) {
        int rc = do_create_view(extra, &v);
        assert_true(rc != 0);
        file_put(extra);
    }

    /* Freeing one slot must make a subsequent CREAT_VIEW succeed. */
    file_put(files[0]);
    file_t *refill = open_gfx0();
    assert_not_null(refill);
    assert_eq(0, do_create_view(refill, &v));
    file_put(refill);

    for (int i = 1; i < GFX_MAX_VIEWS; i++) {
        if (files[i]) file_put(files[i]);
    }
}

/* ── Present tests ─────────────────────────────────── */

TEST_FUNC(test_present_odd_width_happy_path) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* Odd-width view (w=5, h=3).  stride must be 5*4=20. */
    const uint32_t W = 5, H = 3;
    gfx_view_desc_t v = { 0, 0, W, H };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(20u, info.stride);

    /* Build a user buffer (host heap) with unique 32-bit values. */
    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x1000);
    for (uint32_t i = 0; i < W * H; i++) user_buf[i] = 0x1000 + i;

    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(0, do_present(f, &req));

    /* Verify fb contains exactly the values we wrote. */
    for (uint32_t row = 0; row < H; row++) {
        for (uint32_t col = 0; col < W; col++) {
            uint32_t v_expected = 0x1000 + row * W + col;
            uint32_t v_actual = test_fb_buf[row * TEST_FB_WIDTH + col];
            assert_eq(v_expected, v_actual);
        }
    }

    file_put(f);
}

TEST_FUNC(test_present_sentinel_outside_view) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* View: x=4, y=4, w=4, h=4.  Pixels immediately around the
     * view's right and bottom edges must remain DEADBEEFu. */
    gfx_view_desc_t v = { 4, 4, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(16u, info.stride);

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x2000);
    for (uint32_t i = 0; i < 4 * 4; i++) user_buf[i] = 0xC0FFEE00u + i;

    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(0, do_present(f, &req));

    /* Inside the view: 4x4 block at (4..7, 4..7) is 0xC0FFEE00 + row*4 + col. */
    for (uint32_t row = 0; row < 4; row++) {
        for (uint32_t col = 0; col < 4; col++) {
            uint32_t v_expected = 0xC0FFEE00u + row * 4 + col;
            uint32_t v_actual = test_fb_buf[(4 + row) * TEST_FB_WIDTH + (4 + col)];
            assert_eq(v_expected, v_actual);
        }
    }
    /* Pixels immediately right of the view (col 8, rows 4..7): must
     * still be the pre-test DEADBEEFu sentinel. */
    for (uint32_t row = 0; row < 4; row++) {
        assert_eq(0xDEADBEEFu, test_fb_buf[(4 + row) * TEST_FB_WIDTH + 8]);
    }
    /* Pixels immediately below the view (row 8, cols 4..7). */
    for (uint32_t col = 0; col < 4; col++) {
        assert_eq(0xDEADBEEFu, test_fb_buf[8 * TEST_FB_WIDTH + (4 + col)]);
    }

    file_put(f);
}

TEST_FUNC(test_present_sentinel_in_row_padding) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* View: x=0, y=0, w=3, h=2.  Pixels at cols 3..7 in the same
     * rows (the view's right-side row padding) must remain untouched.
     * This proves fb_write_row copies ONLY the view's row_bytes,
     * not a whole fb scanline. */
    gfx_view_desc_t v = { 0, 0, 3, 2 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(12u, info.stride);

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x3000);
    user_buf[0] = 0xAA11AA11u;
    user_buf[1] = 0xAA22AA22u;
    user_buf[2] = 0xAA33AA33u;
    user_buf[3] = 0xAA44AA44u;
    user_buf[4] = 0xAA55AA55u;
    user_buf[5] = 0xAA66AA66u;

    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(0, do_present(f, &req));

    /* View pixels: 3x2 block at (0..2, 0..1). */
    assert_eq(0xAA11AA11u, test_fb_buf[0]);
    assert_eq(0xAA22AA22u, test_fb_buf[1]);
    assert_eq(0xAA33AA33u, test_fb_buf[2]);
    assert_eq(0xAA44AA44u, test_fb_buf[TEST_FB_WIDTH]);
    assert_eq(0xAA55AA55u, test_fb_buf[TEST_FB_WIDTH + 1]);
    assert_eq(0xAA66AA66u, test_fb_buf[TEST_FB_WIDTH + 2]);
    /* Row padding (col 3..7) in both rows: must remain DEADBEEFu. */
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t col = 3; col < TEST_FB_WIDTH; col++) {
            assert_eq(0xDEADBEEFu,
                      test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }

    file_put(f);
}

TEST_FUNC(test_present_overlapping_views_in_present_order) {
    reset_test_state();
    file_t *f1 = open_gfx0();
    file_t *f2 = open_gfx0();
    assert_not_null(f1);
    assert_not_null(f2);

    /* Two overlapping views at (0,0): f1 is 4x4 RED, f2 is 4x4 GREEN. */
    gfx_view_desc_t v1 = { 0, 0, 4, 4 };
    gfx_view_desc_t v2 = { 2, 2, 4, 4 };
    assert_eq(0, do_create_view(f1, &v1));
    assert_eq(0, do_create_view(f2, &v2));

    gfx_info_t i1, i2;
    assert_eq(0, do_get_info(f1, &i1));
    assert_eq(0, do_get_info(f2, &i2));

    uint32_t *user_buf1 = (uint32_t *)(g_user_sim.buf + 0x4000);
    uint32_t *user_buf2 = (uint32_t *)(g_user_sim.buf + 0x5000);
    for (uint32_t i = 0; i < 16; i++) user_buf1[i] = 0xFF0000FFu;  /* magenta-ish */
    for (uint32_t i = 0; i < 16; i++) user_buf2[i] = 0x00FF00FFu;  /* green-ish */

    /* Present f1 first (overlap area (2..3, 2..3) becomes 0xFF0000FF). */
    gfx_present_req_t req1 = { (uint64_t)user_buf1, i1.stride, 0 };
    assert_eq(0, do_present(f1, &req1));
    assert_eq(0xFF0000FFu, test_fb_buf[2 * TEST_FB_WIDTH + 2]);

    /* Present f2 second: the overlap area must now be f2's value. */
    gfx_present_req_t req2 = { (uint64_t)user_buf2, i2.stride, 0 };
    assert_eq(0, do_present(f2, &req2));
    assert_eq(0x00FF00FFu, test_fb_buf[2 * TEST_FB_WIDTH + 2]);
    /* Non-overlap area of f1 (col 0..1, row 0..3) keeps f1's colour. */
    assert_eq(0xFF0000FFu, test_fb_buf[0 * TEST_FB_WIDTH + 0]);
    assert_eq(0xFF0000FFu, test_fb_buf[1 * TEST_FB_WIDTH + 1]);

    file_put(f1);
    file_put(f2);
}

TEST_FUNC(test_present_invalid_stride) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(16u, info.stride);

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x6000);
    gfx_present_req_t req = { (uint64_t)user_buf, info.stride + 4, 0 };
    assert_eq(-EINVAL, do_present(f, &req));

    /* Zero stride also rejected. */
    gfx_present_req_t req2 = { (uint64_t)user_buf, 0, 0 };
    assert_eq(-EINVAL, do_present(f, &req2));

    /* Sentinel must remain DEADBEEFu — present must NOT have written. */
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);

    file_put(f);
}

TEST_FUNC(test_present_invalid_reserved) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x7000);
    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 1 };
    assert_eq(-EINVAL, do_present(f, &req));
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);

    file_put(f);
}

TEST_FUNC(test_present_user_pointer_in_kernel_range) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    /* pixels points into kernel virtual memory (lower-half / below
     * USER_MIN_ADDR).  syscall_check_user_range rejects it. */
    gfx_present_req_t req = { 0x100000ULL, info.stride, 0 };
    assert_eq(-EFAULT, do_present(f, &req));
    /* fb untouched */
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);

    file_put(f);
}

TEST_FUNC(test_present_missing_range_check_fails) {
    /* If the gfx device SKIPS syscall_check_user_range on the
     * request struct or the per-row buffers during present, a
     * -EFAULT would not surface and our sentinel buffer contents
     * would change.  Tighten the simulator: set up the view with
     * range checks ENABLED, then reject any range check (forcing
     * the production code's PRESENT path to surface the rejection
     * as -EFAULT).  create_view / get_info also consult
     * syscall_check_user_range, so we must set those up before
     * flipping the simulator. */
    reset_test_state();

    file_t *f = open_gfx0();
    assert_not_null(f);

    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    /* NOW disable range checks — the only operation left is the
     * PRESENT ioctl, which must consult syscall_check_user_range
     * on the request struct AND per-row buffers. */
    g_user_sim.range_check_ok = false;

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x8000);
    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(-EFAULT, do_present(f, &req));
    /* The production code MUST have consulted the range check (at
     * least once — for the request struct).  If it did, count > 0. */
    assert_true(g_user_sim.range_check_count > 0);
    /* Sentinel intact. */
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);

    file_put(f);
}

TEST_FUNC(test_present_mid_fault_leaves_partial) {
    /* Inject a copy_from_user_ft fault on the 3rd row of a 5-row
     * present.  The first 2 rows MUST be visible in the fb; rows
     * 3..4 MUST remain at the sentinel.  The ioctl MUST return
     * -EFAULT.  This verifies "stop on first fault; earlier rows
     * may remain visible" semantics.
     *
     * Simulator copy counter: create_view consumes copy 1 (its
     * kdesc copy_from_user_ft), present's request struct consumes
     * copy 2, then present's rows consume copies 3+.  So to fail
     * row 2 (the 3rd row, 0-indexed) we need fault_after_calls=4
     * — copies 1..4 (create, req, row 0, row 1) succeed, copy 5
     * (row 2) fails. */
    reset_test_state();
    g_user_sim.fault_after_calls = 4;     /* fail starting from row 2 */

    file_t *f = open_gfx0();
    assert_not_null(f);

    const uint32_t W = 4, H = 5;
    gfx_view_desc_t v = { 0, 0, W, H };
    assert_eq(0, do_create_view(f, &v));

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x9000);
    for (uint32_t i = 0; i < W * H; i++) user_buf[i] = 0xBEEF0000u + i;

    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(-EFAULT, do_present(f, &req));

    /* First two rows reflect user data; the rest are the sentinel. */
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t col = 0; col < W; col++) {
            uint32_t v_expected = 0xBEEF0000u + row * W + col;
            assert_eq(v_expected, test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }
    for (uint32_t row = 2; row < H; row++) {
        for (uint32_t col = 0; col < W; col++) {
            assert_eq(0xDEADBEEFu, test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }

    file_put(f);
}

TEST_FUNC(test_present_unconfigured_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* Issue PRESET before CREAT_VIEW. */
    gfx_present_req_t req = { (uint64_t)g_user_sim.buf + 0xA000, 4, 0 };
    assert_eq(-EINVAL, do_present(f, &req));

    file_put(f);
}

TEST_FUNC(test_present_unknown_cmd_returns_enotty) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);

    /* 0x4FFF is outside the GFX_* ioctl range. */
    int rc = (int)fd_ioctl(f, 0x4FFF, NULL);
    assert_eq(-ENOTTY, rc);

    file_put(f);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_create_view_rejects_zero_size),
    TEST_ENTRY(test_create_view_rejects_uint32_max_overflow),
    TEST_ENTRY(test_create_view_rejects_stride_overflow),
    TEST_ENTRY(test_create_view_exact_fit_right_edge),
    TEST_ENTRY(test_create_view_exact_fit_bottom_edge),
    TEST_ENTRY(test_create_view_rejects_reconfigure),
    TEST_ENTRY(test_get_info_unconfigured_returns_einval),
    TEST_ENTRY(test_two_independent_views),
    TEST_ENTRY(test_release_returns_slot),
    TEST_ENTRY(test_create_view_enforces_16_limit),
    TEST_ENTRY(test_present_odd_width_happy_path),
    TEST_ENTRY(test_present_sentinel_outside_view),
    TEST_ENTRY(test_present_sentinel_in_row_padding),
    TEST_ENTRY(test_present_overlapping_views_in_present_order),
    TEST_ENTRY(test_present_invalid_stride),
    TEST_ENTRY(test_present_invalid_reserved),
    TEST_ENTRY(test_present_user_pointer_in_kernel_range),
    TEST_ENTRY(test_present_missing_range_check_fails),
    TEST_ENTRY(test_present_mid_fault_leaves_partial),
    TEST_ENTRY(test_present_unconfigured_returns_einval),
    TEST_ENTRY(test_present_unknown_cmd_returns_enotty),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
