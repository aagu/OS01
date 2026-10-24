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
#include <setjmp.h>               // sigjmp_buf for the host fault bridge

#include <driver/gfx.h>
#include <driver/fb.h>
#include <uapi/gfx.h>
#include <fs/file.h>
#include <fs/devfs.h>
#include <core/printk.h>          // position Pos (host-test fake)
#include <memory/uaccess.h>       // USER_MIN_ADDR (simulator matches it)
#include <lwip/err.h>
#include <lwip/api.h>

/* Test-side instrumentation globals (declared early so the
 * mocks below can reference them). */
static int g_kmalloc_fail_after = -1;   /* -1 disables; N fails the Nth+1 kmalloc */
static int g_kmalloc_calls;
static int g_fake_lease_rc;            /* 0 = normal; otherwise injected errno */
static uint32_t g_write_row_calls;
static uint32_t g_write_row_total_bytes;
static uint32_t g_write_row_min_bytes = UINT32_MAX;
static uint32_t g_write_row_max_bytes;

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

/* kmalloc / kfree — host wrappers with a test-only failure
 * injector (Task 1 Step 5).  Production callers see plain malloc;
 * the failure counter advances on every call so a single test can
 * inject ENOMEM at the bounce-kmalloc step. */
void *kmalloc(size_t size) {
    g_kmalloc_calls++;
    if (g_kmalloc_fail_after >= 0 && g_kmalloc_calls > g_kmalloc_fail_after) {
        return NULL;
    }
    return malloc(size ? size : 1);
}
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

int vfs_mount(const char *p, block_device_t *d,
              const vfs_inode_ops_t *io, const vfs_file_ops_t *fo, void *f)
{ (void)p; (void)d; (void)io; (void)fo; (void)f; return -ENOSYS; }
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

/* arch_clocksource_read_ns — production gfx.c samples wall time
 * around the present path (terminal-cpu probe, retained long-term).
 * The host harness doesn't care about the value, only that the
 * symbol resolves. */
uint64_t arch_clocksource_read_ns(void) { return 0; }

/* log_debug is a macro in production that expands to log(LOG_DEBUG, ...),
 * which references g_log_level and _log_write.  The test's own
 * log_debug stub satisfies the prototype; we still need the two
 * backing symbols so the production macro expansion links.
 *
 * LOG_DEBUG = 7 (kernel/include/log/log.h).  Set g_log_level below
 * the LOG_DEBUG threshold so the kernel stats path is silent in the
 * host harness — host tests assert on test-side counters, not on
 * log lines. */
int g_log_level = 0;       /* below LOG_DEBUG (7) — every log_debug
                            * expands to do {} while(0) at runtime. */
void _log_write(int level, const char *fmt, ...)
{ (void)level; (void)fmt; }

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

static uint64_t test_mode_generation = 1;

int fb_snapshot_read(fb_snapshot_t *out)
{
    if (!out) return -EINVAL;
    out->state.info.width  = test_fb_width_override ? test_fb_width_override
                                                    : (uint32_t)TEST_FB_WIDTH;
    out->state.info.height = (uint32_t)TEST_FB_HEIGHT;
    out->state.info.stride = TEST_FB_STRIDE;
    out->state.info.bpp    = 32;
    out->state.info.format = GFX_FORMAT_RGB32;
    out->state.generation  = test_mode_generation;
    out->state.reserved    = 0;
    out->addr = test_fb_buf;
    out->mapped_size = TEST_FB_SIZE;
    return 0;
}

static const void *g_test_owner = NULL;
static const void *g_test_fallback = NULL;
static uint64_t g_test_owner_generation = 1;
static uint32_t g_test_active_writers = 0;

int fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation)
{
    if (!lease) return -EINVAL;
    int rc = fb_snapshot_read(&lease->snapshot);
    if (rc < 0) return rc;
    if (expected_generation != 0 && expected_generation != lease->snapshot.state.generation)
        return -ESTALE;
    lease->held = true;
    g_test_active_writers++;
    return 0;
}

int fb_gfx_writer_begin(fb_lease_t *lease, uint64_t mode_seq, const void *cookie)
{
    if (!lease) return -EINVAL;
    if (g_test_owner != cookie) return -EACCES;
    if (g_fake_lease_rc != 0) return g_fake_lease_rc;
    int rc = fb_snapshot_read(&lease->snapshot);
    if (rc < 0) return rc;
    if (mode_seq != 0 && mode_seq != lease->snapshot.state.generation)
        return -ESTALE;
    lease->held = true;
    g_test_active_writers++;
    return 0;
}

void fb_writer_end(fb_lease_t *lease)
{
    if (lease && lease->held) {
        lease->held = false;
        if (g_test_active_writers > 0) g_test_active_writers--;
    }
}

int fb_owner_acquire(const void *cookie, uint64_t expected_mode)
{
    if (!cookie) return -EINVAL;
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) return rc;
    if (expected_mode != 0 && expected_mode != snap.state.generation)
        return -ESTALE;
    if (g_test_owner != NULL && g_test_owner != g_test_fallback && g_test_owner != cookie)
        return -EBUSY;
    if (g_test_owner != cookie) {
        g_test_owner = cookie;
        g_test_owner_generation++;
    }
    return 0;
}

int fb_owner_release(const void *cookie)
{
    if (!cookie) return -EINVAL;
    if (g_test_owner == cookie && g_test_owner != g_test_fallback) {
        g_test_owner = g_test_fallback;
        g_test_owner_generation++;
    }
    return 0;
}

int fb_owner_register_fallback(const void *cookie, uint64_t expected_mode)
{
    if (!cookie) return -EINVAL;
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) return rc;
    if (expected_mode != 0 && expected_mode != snap.state.generation)
        return -ESTALE;
    if (g_test_fallback != NULL && g_test_fallback != cookie)
        return -EBUSY;
    g_test_fallback = cookie;
    if (g_test_owner == NULL) {
        g_test_owner = cookie;
        g_test_owner_generation++;
    }
    return 0;
}

int fb_owner_replace_fallback(const void *old_cookie, const void *new_cookie, uint64_t expected_mode)
{
    if (!old_cookie || !new_cookie) return -EINVAL;
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    if (rc < 0) return rc;
    if (expected_mode != 0 && expected_mode != snap.state.generation)
        return -ESTALE;
    if (g_test_fallback != old_cookie) return -EINVAL;
    g_test_fallback = new_cookie;
    if (g_test_owner == old_cookie) g_test_owner = new_cookie;
    g_test_owner_generation++;
    return 0;
}

void fb_owner_detach(const void *cookie)
{
    if (!cookie) return;
    bool changed = false;
    if (g_test_fallback == cookie) { g_test_fallback = NULL; changed = true; }
    if (g_test_owner == cookie) {
        g_test_owner = g_test_fallback;
        changed = true;
    }
    if (changed) g_test_owner_generation++;
}

int fb_owner_get_state(const void *cookie, gfx_display_state_t *out)
{
    if (!out) return -EINVAL;
    uint32_t flags = 0;
    if (cookie && g_test_owner == cookie) flags |= GFX_DISPLAY_STATE_FOREGROUND;
    if (cookie && g_test_fallback == cookie) flags |= GFX_DISPLAY_STATE_FALLBACK;
    out->generation = g_test_owner_generation;
    out->flags = flags;
    out->reserved = 0;
    return 0;
}

/* The input lease is not part of this fixture: the bind ioctl refuses
 * the lease and the unbind is a no-op. */
int fb_input_bind(const void *view, uint64_t expected_mode,
                  struct file *kbd, struct file *mouse,
                  uint64_t *generation, uint32_t *initial_modifiers)
{
    (void)view; (void)expected_mode; (void)kbd; (void)mouse;
    (void)generation; (void)initial_modifiers;
    return -ENODEV;
}

void fb_input_unbind(const void *view, uint64_t generation, int reason)
{
    (void)view; (void)generation; (void)reason;
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

/* fb_write_row_leased — production uses this for both full-frame
 * (per-row, stride bytes) and rect (per-row, w*4 bytes).  Counting
 * here lets the rect tests assert "begin/end each called once" and
 * "total write bytes match requested geometry". */
int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y,
                        const void *pixels, uint32_t bytes)
{
    (void)lease;
    g_write_row_calls++;
    g_write_row_total_bytes += bytes;
    if (bytes < g_write_row_min_bytes) g_write_row_min_bytes = bytes;
    if (bytes > g_write_row_max_bytes) g_write_row_max_bytes = bytes;
    return fb_write_row(x, y, pixels, bytes);
}

/* ── Present-counting shim ──
 * The production code uses fb_write_row_leased for both the full
 * path (per-row, stride bytes) and the rect path (per-row, w*4
 * bytes).  We need a test-visible counter so the rect tests can
 * assert "begin/end each called once" + "total write bytes match
 * the requested geometry".  fb_write_row_leased is mocked here so we
 * can observe its call count and the sum of row_bytes without
 * affecting the fb contents.  (Globals declared at the top of the
 * file so the production gfx.c's call path can update them.) */

static void reset_write_counters(void)
{
    g_write_row_calls = 0;
    g_write_row_total_bytes = 0;
    g_write_row_min_bytes = UINT32_MAX;
    g_write_row_max_bytes = 0;
}

/* ── Host SIGSEGV bridge plumbing (Task 1 Step 2) ──
 * The bridge translates a real SIGSEGV on a PROT_NONE region into
 * the production kernel's fault_jmp longjmp.  This is the ONLY way
 * to fault the inline __builtin_memcpy that gfx.c uses after the
 * v1 terminal-cpu probe (the user_sim.fault_after_calls mock only
 * intercepts copy_*_ft).  Production semantics: fault → EFAULT,
 * earlier rows visible, current->fault_jmp / fault_cleanup /
 * fault_cleanup_arg restored to their pre-arm values, bounce and
 * lease released. */
#include "gfx_fault_bridge.h"
#include <sys/mman.h>

/* Per-test handle the bridge passes back to our get_jump.  The
 * bridge reads current->fault_jmp through this callback so the
 * production kernel's longjmp entry is hit.  The bridge .c uses
 * __builtin_longjmp(slot, 1) — the SAME clang compiler builtin
 * the production gfx_ioctl_present_rect uses for __builtin_setjmp,
 * so the layout match is guaranteed by construction. */
struct bridge_ctx {
    task_t *task;
};
static struct bridge_ctx g_bridge_ctx;

static void **test_get_jump(void *user_ctx)
{
    struct bridge_ctx *ctx = (struct bridge_ctx *)user_ctx;
    if (!ctx->task) return NULL;
    /* Return current->fault_jmp: the production gfx.c armed it
     * to point at its own os01_jmp_buf (= void *[8]).  The bridge
     * calls __builtin_longjmp on this exact buffer. */
    return (void **)ctx->task->fault_jmp;
}

/* The test's own setjmp slot — paired with the bridge's
 * __builtin_longjmp.  The bridge longjmps back here with value 1.
 * Same TU compile options as the bridge .c, so __builtin_setjmp
 * here and __builtin_longjmp there agree on the buffer layout. */
static void *g_test_jmp_buf[8];

/* ── Test runtime helpers ── */

static void reset_test_state(void)
{
    test_fb_width_override = 0;
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
    /* Fault recovery slots start NULL — the production code arms
     * them per call and restores them on every return path. */
    test_task.fault_jmp = NULL;
    test_task.fault_cleanup = NULL;
    test_task.fault_cleanup_arg = NULL;
    jiffies = 0;

    /* Default: no kmalloc / lease injection. */
    g_kmalloc_fail_after = -1;
    g_kmalloc_calls = 0;
    g_fake_lease_rc = 0;
    reset_write_counters();
    memset(&g_bridge_ctx, 0, sizeof(g_bridge_ctx));

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

    g_test_owner = NULL;
    g_test_fallback = NULL;
    g_test_owner_generation = 1;
    g_test_active_writers = 0;
    test_mode_generation = 1;
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

static int do_present_rect(file_t *f, gfx_present_rect_req_t *req)
{
    return (int)fd_ioctl(f, GFX_PRESENT_RECT, (void *)req);
}

static int do_acquire(file_t *f)
{
    return (int)fd_ioctl(f, GFX_ACQUIRE, NULL);
}

static int do_release(file_t *f)
{
    return (int)fd_ioctl(f, GFX_RELEASE, NULL);
}

static int do_register_fallback(file_t *f)
{
    return (int)fd_ioctl(f, GFX_REGISTER_FALLBACK, NULL);
}

static int do_get_display_state(file_t *f, gfx_display_state_t *st)
{
    return (int)fd_ioctl(f, GFX_GET_DISPLAY_STATE, (void *)st);
}

static int do_replace_fallback(file_t *f, gfx_replace_fallback_t *req)
{
    return (int)fd_ioctl(f, GFX_REPLACE_FALLBACK, (void *)req);
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

    assert_eq(0, do_acquire(f));

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

    assert_eq(0, do_acquire(f));

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

    assert_eq(0, do_acquire(f));

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
    assert_eq(0, do_acquire(f1));
    gfx_present_req_t req1 = { (uint64_t)user_buf1, i1.stride, 0 };
    assert_eq(0, do_present(f1, &req1));
    assert_eq(0xFF0000FFu, test_fb_buf[2 * TEST_FB_WIDTH + 2]);
    assert_eq(0, do_release(f1));

    /* Present f2 second: the overlap area must now be f2's value. */
    assert_eq(0, do_acquire(f2));
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
    /* Mid-fault coverage for the full-frame present path.
     *
     * The production code uses inline __builtin_memcpy guarded by
     * current->fault_jmp.  The legacy user_sim.fault_after_calls
     * mock only intercepts copy_*_ft (the request struct copy),
     * not the inline memcpy — so to fault mid-loop we need the
     * host SIGSEGV bridge.  The bridge's longjmp layout
     * compatibility with the production TU's setjmp is host-
     * compiler-version-sensitive and is currently a known
     * limitation; this test therefore exercises the request-copy
     * fault path through the old mock (per the brief: "请求copy
     * fault沿用旧mock").
     *
     * Inject the fault just past the request-struct copy: the
     * first copy_from_user_ft (request struct) succeeds; any
     * later copy would fail.  Since the inline-memcpy path makes
     * no further copy_*_ft calls, we instead target the request
     * struct itself with fault_after_calls = 0 — but that also
     * faults create_view, so we configure the view BEFORE
     * enabling the fault.
     *
     * Verified contract: request-struct fault → ioctl returns
     * -EFAULT, lease released, fb untouched. */
    reset_test_state();

    file_t *f = open_gfx0();
    assert_not_null(f);
    const uint32_t W = 4, H = 5;
    gfx_view_desc_t v = { 0, 0, W, H };
    assert_eq(0, do_create_view(f, &v));
    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(0, do_acquire(f));

    /* Enable the fault NOW — the next copy_from_user_ft (the
     * request struct of the upcoming present) will fail. */
    g_user_sim.fault_after_calls = 0;

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x9000);
    for (uint32_t i = 0; i < W * H; i++) user_buf[i] = 0xBEEF0000u + i;

    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(-EFAULT, do_present(f, &req));

    /* No fb writes: every cell still at the pre-test sentinel. */
    for (uint32_t row = 0; row < H; row++) {
        for (uint32_t col = 0; col < W; col++) {
            assert_eq(0xDEADBEEFu, test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }
    assert_eq(0, g_test_active_writers);

    /* Clear the fault so subsequent tests run normally. */
    g_user_sim.fault_after_calls = -1;

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

TEST_FUNC(test_acquire_unconfigured_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    assert_eq(-EINVAL, do_acquire(f));
    file_put(f);
}

TEST_FUNC(test_acquire_stale_mode_returns_estale) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));

    /* View was created at mode generation 1. Simulate mode bump: */
    test_mode_generation = 2;
    assert_eq(-ESTALE, do_acquire(f));

    file_put(f);
}

TEST_FUNC(test_register_fallback_unconfigured_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    assert_eq(-EINVAL, do_register_fallback(f));
    file_put(f);
}

TEST_FUNC(test_register_fallback_not_fullscreen_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));
    assert_eq(-EINVAL, do_register_fallback(f));
    file_put(f);
}

TEST_FUNC(test_get_display_state_unconfigured_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_display_state_t st;
    assert_eq(-EINVAL, do_get_display_state(f, &st));
    file_put(f);
}

TEST_FUNC(test_get_display_state_fault_returns_efault) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));
    assert_eq(-EFAULT, do_get_display_state(f, NULL));
    assert_eq(-EFAULT, do_get_display_state(f, (gfx_display_state_t *)0x1000ULL));
    file_put(f);
}

TEST_FUNC(test_replace_fallback_reserved_nonzero_returns_einval) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f, &v));
    gfx_replace_fallback_t req = { .old_fd = 0, .reserved = 1 };
    assert_eq(-EINVAL, do_replace_fallback(f, &req));
    file_put(f);
}

TEST_FUNC(test_replace_fallback_invalid_old_fd_returns_ebadf) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f, &v));
    gfx_replace_fallback_t req = { .old_fd = -1, .reserved = 0 };
    assert_eq(-EBADF, do_replace_fallback(f, &req));
    gfx_replace_fallback_t req2 = { .old_fd = 15, .reserved = 0 };
    assert_eq(-EBADF, do_replace_fallback(f, &req2));
    file_put(f);
}

TEST_FUNC(test_replace_fallback_fault_returns_efault) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f, &v));
    assert_eq(-EFAULT, do_replace_fallback(f, NULL));
    assert_eq(-EFAULT, do_replace_fallback(f, (gfx_replace_fallback_t *)0x1000ULL));
    file_put(f);
}

TEST_FUNC(test_replace_fallback_non_gfx_fd_returns_einval) {
    reset_test_state();
    file_t *dummy = file_alloc();
    dummy->type = FD_PIPE;
    test_files.fd[2] = dummy;

    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f, &v));

    gfx_replace_fallback_t req = { .old_fd = 2, .reserved = 0 };
    assert_eq(-EINVAL, do_replace_fallback(f, &req));

    file_put(f);
    test_files.fd[2] = NULL;
    file_put(dummy);
}

TEST_FUNC(test_replace_fallback_null_node_returns_einval) {
    reset_test_state();
    file_t *f_old = open_gfx0();
    assert_not_null(f_old);
    gfx_view_desc_t v_old = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f_old, &v_old));
    assert_eq(0, do_register_fallback(f_old));

    file_t *dummy = file_alloc();
    dummy->type = FD_DEV;
    dummy->node = NULL;
    dummy->dev_private = f_old->dev_private;
    test_files.fd[2] = dummy;

    file_t *f_new = open_gfx0();
    assert_not_null(f_new);
    gfx_view_desc_t v_new = { 0, 0, TEST_FB_WIDTH, TEST_FB_HEIGHT };
    assert_eq(0, do_create_view(f_new, &v_new));

    gfx_replace_fallback_t req = { .old_fd = 2, .reserved = 0 };
    assert_eq(-EINVAL, do_replace_fallback(f_new, &req));

    file_put(f_new);
    test_files.fd[2] = NULL;
    file_put(dummy);
    file_put(f_old);
}

TEST_FUNC(test_present_not_owner_returns_eacces_and_no_writes) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));
    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    /* View is NOT owner */
    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x8000);
    for (int i = 0; i < 16; i++) user_buf[i] = 0x11223344;
    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };

    int rc = do_present(f, &req);
    assert_eq(-EACCES, rc);
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);
    assert_eq(0, g_test_active_writers);

    file_put(f);
}

TEST_FUNC(test_acquire_and_present_success) {
    reset_test_state();
    file_t *f = open_gfx0();
    assert_not_null(f);
    gfx_view_desc_t v = { 0, 0, 4, 4 };
    assert_eq(0, do_create_view(f, &v));
    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    assert_eq(0, do_acquire(f));

    gfx_display_state_t st;
    assert_eq(0, do_get_display_state(f, &st));
    assert_eq(GFX_DISPLAY_STATE_FOREGROUND, st.flags & GFX_DISPLAY_STATE_FOREGROUND);

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0x8000);
    for (int i = 0; i < 16; i++) user_buf[i] = 0xCAFEBABE;
    gfx_present_req_t req = { (uint64_t)user_buf, info.stride, 0 };
    assert_eq(0, do_present(f, &req));
    assert_eq(0xCAFEBABEu, test_fb_buf[0]);
    assert_eq(0, g_test_active_writers);

    file_put(f);
}

/* ── Partial-rectangle present tests (Task 1) ── */

/* Helper: configure a view at view=(4,5,8,6) so that the local
 * rect=(2,1,3,2) maps to framebuffer coords (4+2, 5+1) .. (4+2+3,
 * 5+1+2) = (6..9, 6..8).  Returns the configured file. */
static file_t *open_view_4_5_8_6(void)
{
    file_t *f = open_gfx0();
    if (!f) return NULL;
    gfx_view_desc_t v = { 4, 5, 8, 6 };
    if (do_create_view(f, &v) != 0) {
        file_put(f);
        return NULL;
    }
    gfx_info_t info;
    if (do_get_info(f, &info) != 0) {
        file_put(f);
        return NULL;
    }
    if (do_acquire(f) != 0) {
        file_put(f);
        return NULL;
    }
    return f;
}

/* Fill the test user buffer (at the given byte offset) with a
 * deterministic per-view-cell value: pixel at view-local (col, row)
 * gets 0xCAFE0000u + row * 8 + col. */
static void fill_user_buf_pattern(uint32_t *user_buf)
{
    for (uint32_t r = 0; r < 6; r++) {
        for (uint32_t c = 0; c < 8; c++) {
            user_buf[r * 8 + c] = 0xCAFE0000u + r * 8 + c;
        }
    }
}

/* Step 1, case A: 3x2 rect happy path.  64x48 fake FB; view at
 * (4,5,8,6); rect=(2,1,3,2).  Expected:
 *     - FB pixels at x=6..8, y=6..7 reflect the matching local
 *       cells from the user buffer;
 *     - every other pixel (including rect-adjacent sentinels)
 *       remains 0xDEADBEEFu;
 *     - exactly one fb_gfx_writer_begin / fb_writer_end pair (we
 *       proxy via g_test_active_writers==0 after the call);
 *     - 2 fb_write_row_leased calls (one per row);
 *     - row_bytes == w*4 == 12, total write bytes == 24. */
TEST_FUNC(test_present_rect_offsets_and_sentinel) {
    reset_test_state();
    file_t *f = open_view_4_5_8_6();
    assert_not_null(f);

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq(32u, info.stride);

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0xA000);
    fill_user_buf_pattern(user_buf);

    gfx_present_rect_req_t req = {
        .pixels   = (uint64_t)user_buf,
        .stride   = info.stride,
        .reserved = 0,
        .x        = 2, .y = 1,
        .w        = 3, .h = 2,
    };
    assert_eq(0, do_present_rect(f, &req));

    /* Exactly the rect pixels (FB coords x=6..8, y=6..7) match the
     * local cells (x=2..4, y=1..2) of the user buffer. */
    for (uint32_t row = 0; row < 2; row++) {
        for (uint32_t col = 0; col < 3; col++) {
            uint32_t v_expected = 0xCAFE0000u + (row + 1) * 8 + (col + 2);
            uint32_t v_actual   = test_fb_buf[(6 + row) * TEST_FB_WIDTH + (6 + col)];
            assert_eq(v_expected, v_actual);
        }
    }
    /* Rect-adjacent sentinels remain 0xDEADBEEFu.  Row 6 col 5
     * (just before the rect's left edge) and col 9 (just past the
     * rect's right edge); row 5 (just before the rect's top edge)
     * and row 8 (just past the bottom edge) at x=6..8. */
    for (uint32_t row = 0; row < 2; row++) {
        assert_eq(0xDEADBEEFu, test_fb_buf[(6 + row) * TEST_FB_WIDTH + 5]);
        assert_eq(0xDEADBEEFu, test_fb_buf[(6 + row) * TEST_FB_WIDTH + 9]);
    }
    for (uint32_t col = 0; col < 3; col++) {
        assert_eq(0xDEADBEEFu, test_fb_buf[5 * TEST_FB_WIDTH + 6 + col]);
        assert_eq(0xDEADBEEFu, test_fb_buf[8 * TEST_FB_WIDTH + 6 + col]);
    }
    /* Write counters. */
    assert_eq(2u, g_write_row_calls);
    assert_eq(24u, g_write_row_total_bytes);
    assert_eq(12u, g_write_row_min_bytes);
    assert_eq(12u, g_write_row_max_bytes);
    /* Lease released exactly once. */
    assert_eq(0, g_test_active_writers);

    file_put(f);
}

/* Step 1, case B: 1x1 bottom-right corner — pinpoints the
 * boundary check.  view=(4,5,8,6) -> x range 4..11, y range 5..10.
 * rect=(7,5,1,1) -> FB pixel (11,10) only. */
TEST_FUNC(test_present_rect_one_pixel_bottom_right_corner) {
    reset_test_state();
    file_t *f = open_view_4_5_8_6();
    assert_not_null(f);

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0xB000);
    fill_user_buf_pattern(user_buf);

    /* pixel at local (7,5) = 0xCAFE0000 + 5*8 + 7 = 0xCAFE002F */
    gfx_present_rect_req_t req = {
        .pixels   = (uint64_t)user_buf,
        .stride   = info.stride,
        .reserved = 0,
        .x        = 7, .y = 5,
        .w        = 1, .h = 1,
    };
    assert_eq(0, do_present_rect(f, &req));

    assert_eq(0xCAFE002Fu, test_fb_buf[10 * TEST_FB_WIDTH + 11]);
    /* Adjacent pixels untouched. */
    assert_eq(0xDEADBEEFu, test_fb_buf[10 * TEST_FB_WIDTH + 10]);
    assert_eq(0xDEADBEEFu, test_fb_buf[10 * TEST_FB_WIDTH + 12]);
    assert_eq(0xDEADBEEFu, test_fb_buf[9  * TEST_FB_WIDTH + 11]);
    assert_eq(0xDEADBEEFu, test_fb_buf[11 * TEST_FB_WIDTH + 11]);
    /* Counts: 1 row, 4 bytes. */
    assert_eq(1u, g_write_row_calls);
    assert_eq(4u, g_write_row_total_bytes);
    assert_eq(4u, g_write_row_min_bytes);
    assert_eq(4u, g_write_row_max_bytes);

    file_put(f);
}

/* Step 1, case C: full-view rect == view dimensions.  Must produce
 * the same fb state as a full-frame present (same view stride, same
 * w*h).  Does NOT share code with the full path (separate ioctl). */
TEST_FUNC(test_present_rect_full_view_dimensions) {
    reset_test_state();
    file_t *f = open_view_4_5_8_6();
    assert_not_null(f);

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0xC000);
    fill_user_buf_pattern(user_buf);

    gfx_present_rect_req_t req = {
        .pixels   = (uint64_t)user_buf,
        .stride   = info.stride,
        .reserved = 0,
        .x        = 0, .y = 0,
        .w        = 8, .h = 6,
    };
    assert_eq(0, do_present_rect(f, &req));

    /* Every fb pixel inside (4,5,8,6) matches the corresponding
     * local cell; everything outside is sentinel. */
    for (uint32_t r = 0; r < 6; r++) {
        for (uint32_t c = 0; c < 8; c++) {
            uint32_t v_expected = 0xCAFE0000u + r * 8 + c;
            uint32_t v_actual   = test_fb_buf[(5 + r) * TEST_FB_WIDTH + (4 + c)];
            assert_eq(v_expected, v_actual);
        }
    }
    /* Sentinel just outside the view. */
    assert_eq(0xDEADBEEFu, test_fb_buf[5  * TEST_FB_WIDTH + 3]);
    assert_eq(0xDEADBEEFu, test_fb_buf[5  * TEST_FB_WIDTH + 12]);
    assert_eq(0xDEADBEEFu, test_fb_buf[11 * TEST_FB_WIDTH + 4]);
    assert_eq(0xDEADBEEFu, test_fb_buf[4  * TEST_FB_WIDTH + 4]);
    /* Counts: 6 rows, stride=32 => 192 bytes total. */
    assert_eq(6u, g_write_row_calls);
    assert_eq(192u, g_write_row_total_bytes);
    assert_eq(32u, g_write_row_min_bytes);
    assert_eq(32u, g_write_row_max_bytes);

    file_put(f);
}

/* Step 1: invalid geometry / reserved / stride / pointer cases.
 * Each sub-section closes with a sentinel check + lease-clean check. */
TEST_FUNC(test_present_rect_invalid_geometry) {
    reset_test_state();
    file_t *f = open_view_4_5_8_6();
    assert_not_null(f);

    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));

    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0xD000);
    fill_user_buf_pattern(user_buf);

    /* Zero w → -EINVAL. */
    gfx_present_rect_req_t zero_w = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 0, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &zero_w));
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);
    assert_eq(0, g_test_active_writers);

    /* Zero h → -EINVAL. */
    gfx_present_rect_req_t zero_h = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 0,
    };
    assert_eq(-EINVAL, do_present_rect(f, &zero_h));

    /* UINT32_MAX w with x=0 → w > view_w-x (subtraction form). */
    gfx_present_rect_req_t huge_w = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = UINT32_MAX, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &huge_w));

    /* x+1 + UINT32_MAX w → x <= view_w && w > view_w-x rejected. */
    gfx_present_rect_req_t huge_w_off = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 1, .y = 0, .w = UINT32_MAX, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &huge_w_off));

    /* Same overflow pair on the y/h axis. */
    gfx_present_rect_req_t huge_h = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = UINT32_MAX,
    };
    assert_eq(-EINVAL, do_present_rect(f, &huge_h));
    gfx_present_rect_req_t huge_h_off = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 1, .w = 1, .h = UINT32_MAX,
    };
    assert_eq(-EINVAL, do_present_rect(f, &huge_h_off));

    /* x == view_w (off-by-one past the right edge) → -EINVAL. */
    gfx_present_rect_req_t x_past = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 8, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &x_past));

    /* y == view_h → -EINVAL. */
    gfx_present_rect_req_t y_past = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 6, .w = 1, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &y_past));

    /* reserved != 0 → -EINVAL. */
    gfx_present_rect_req_t reserved_nz = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 1, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &reserved_nz));

    /* Stride mismatch (view_w*4 is 32, anything else rejected). */
    gfx_present_rect_req_t bad_stride = {
        .pixels = (uint64_t)user_buf, .stride = info.stride + 4,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &bad_stride));
    gfx_present_rect_req_t zero_stride = {
        .pixels = (uint64_t)user_buf, .stride = 0,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EINVAL, do_present_rect(f, &zero_stride));

    /* pixels == 0 → -EFAULT. */
    gfx_present_rect_req_t zero_pixels = {
        .pixels = 0, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EFAULT, do_present_rect(f, &zero_pixels));

    /* Kernel-range pointer (below USER_MIN_ADDR) → -EFAULT. */
    gfx_present_rect_req_t kr_pixels = {
        .pixels = 0x100000ULL, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EFAULT, do_present_rect(f, &kr_pixels));

    /* Sentinel still intact after every rejection. */
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);
    assert_eq(0xDEADBEEFu, test_fb_buf[(6) * TEST_FB_WIDTH + 6]);
    /* No writer left stuck. */
    assert_eq(0, g_test_active_writers);

    file_put(f);
}

/* Step 1: error / cleanup contract.  Each lease errno propagates,
 * kmalloc failure leaves the lease released, write-row failure
 * returns the lease errno, and an unconfigured view rejects
 * GFX_PRESENT_RECT before the lease is acquired. */
TEST_FUNC(test_present_rect_errors_and_cleanup) {
    reset_test_state();
    file_t *f = open_view_4_5_8_6();
    assert_not_null(f);
    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    uint32_t *user_buf = (uint32_t *)(g_user_sim.buf + 0xE000);
    fill_user_buf_pattern(user_buf);

    /* Lease returning -ESTALE — the request struct still gets
     * copied; the lease error propagates. */
    g_fake_lease_rc = -ESTALE;
    gfx_present_rect_req_t req = {
        .pixels = (uint64_t)user_buf, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-ESTALE, do_present_rect(f, &req));
    assert_eq(0, g_test_active_writers);
    assert_eq(0xDEADBEEFu, test_fb_buf[0]);

    /* Lease returning -EACCES — same path, different errno. */
    g_fake_lease_rc = -EACCES;
    assert_eq(-EACCES, do_present_rect(f, &req));
    assert_eq(0, g_test_active_writers);

    /* Lease returning -EAGAIN. */
    g_fake_lease_rc = -EAGAIN;
    assert_eq(-EAGAIN, do_present_rect(f, &req));
    assert_eq(0, g_test_active_writers);

    /* Lease returning -EIO. */
    g_fake_lease_rc = -EIO;
    assert_eq(-EIO, do_present_rect(f, &req));
    assert_eq(0, g_test_active_writers);

    /* No lease failure now: kmalloc failure during bounce alloc.
     * The first kmalloc call after we enable the injection is the
     * bounce allocation (the test TU's kmalloc is hooked).  We let
     * the request struct copy succeed (zero kmallocs), then force
     * the next kmalloc to fail. */
    g_fake_lease_rc = 0;
    g_kmalloc_fail_after = 0;   /* fail the very next kmalloc */
    assert_eq(-ENOMEM, do_present_rect(f, &req));
    assert_eq(0, g_test_active_writers);
    /* Disable the failure so subsequent calls work normally. */
    g_kmalloc_fail_after = -1;

    /* Normal present after a sequence of failures: lease + kmalloc
     * both succeed, fb pixels change. */
    assert_eq(0, do_present_rect(f, &req));
    assert_eq(0xCAFE0000u, test_fb_buf[(5 + 0) * TEST_FB_WIDTH + (4 + 0)]);
    assert_eq(0, g_test_active_writers);

    /* Unconfigured view → -EINVAL, no lease acquired. */
    file_t *unf = open_gfx0();
    assert_not_null(unf);
    assert_eq(-EINVAL, do_present_rect(unf, &req));
    assert_eq(0, g_test_active_writers);
    file_put(unf);

    /* Bad request pointer (kernel-range) → -EFAULT. */
    gfx_present_rect_req_t req_kr = {
        .pixels = 0x100000ULL, .stride = info.stride,
        .reserved = 0, .x = 0, .y = 0, .w = 1, .h = 1,
    };
    assert_eq(-EFAULT, do_present_rect(f, &req_kr));

    file_put(f);
}

/* Step 1 + Step 2 (combined): mid-fault via host SIGSEGV bridge.
 *
 * The production gfx_ioctl_present_rect arms current->fault_jmp
 * around an inline __builtin_memcpy.  To fault the memcpy on
 * the host we install a SIGSEGV handler that longjmps back to
 * the test's own __builtin_setjmp site.  The bridge .c uses
 * __builtin_longjmp — the EXACT pair of __builtin_setjmp that
 * production gfx.c uses (same compiler builtin, same layout).
 * The producer's fault_jmp slot is exactly the right buffer for
 * the bridge to longjmp through: the test_get_jump callback
 * returns current->fault_jmp, which the production code armed
 * to point at its own os01_jmp_buf.
 *
 * Layout (W=64, H=4, stride=256) keeps each row 256 bytes apart
 * so AVX-512's wide loads (up to 64 B/load) can't span a row
 * boundary — the protected page fault is pinned to a specific
 * row's source.
 *
 * Two independent protected-region fault tests cover the brief's
 * "连续两次独立保护区fault测试" requirement: a fault on the
 * row-N+1 source (first test), and a fault on the row-N+2
 * source after a different row got visible (second test).  Both
 * are followed by a normal-present sanity check to prove the
 * bridge state is fully reset. */
TEST_FUNC(test_present_rect_mid_fault_with_bridge) {
    /* Independent fault run #1: fault on row 2's source (rect=(0,1,W,3)
     * → view rows 1, 2, 3 → fb rows 1, 2, 3).  Row 0 source
     * = pixels + stride + x*4 = page-end of writable page 0;
     * row 1 source = page 1 = PROT_NONE → fault. */
    reset_test_state();

    file_t *f = open_gfx0();
    assert_not_null(f);
    const uint32_t W = TEST_FB_WIDTH, H = 4;
    /* Use a view that fits inside the 64x48 fake FB.  Width = full
     * 64, height = 4, origin (0, 0).  Stride = 64*4 = 256. */
    gfx_view_desc_t v = { 0, 0, W, H };
    assert_eq(0, do_create_view(f, &v));
    gfx_info_t info;
    assert_eq(0, do_get_info(f, &info));
    assert_eq((unsigned)(W * 4), info.stride);
    assert_eq(0, do_acquire(f));

    size_t page = 4096;
    /* Two-page region: page 0 writable, page 1 PROT_NONE. */
    uint8_t *region = (uint8_t *)mmap(NULL, 2 * page,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);
    assert_true(region != MAP_FAILED);
    assert_eq(0, mprotect(region + page, page, PROT_NONE));

    /* rect=(0,1,W,3) ⇒ row i source = pixels + (1+i)*256 + 0*4
     *                            = pixels + (1+i)*256.
     * Place pixels = region + page - 512 (so row 0 source entirely
     * fits inside writable page 0 with 64 B headroom for AVX-512,
     * and row 1 source starts at the first byte of PROT_NONE page 1):
     *   row 0 src = region + page - 256   (last 256 B of page 0)
     *   row 1 src = region + page          (PROT_NONE — fault target)
     *   row 2 src = region + page + 256    (PROT_NONE)
     * Row 0's wide-reads (up to 64 B/load) end at region + page - 1
     * (last byte of writable page); they never touch the PROT_NONE
     * page.  Row 1's memcpy starts at region + page and faults. */
    uint8_t *pixels = region + page - 512;
    uint32_t *row0_pix = (uint32_t *)(pixels + 256);
    for (uint32_t c = 0; c < W; c++) {
        row0_pix[c] = 0xCAFE0000u + c;
    }

    /* Install the bridge on the PROT_NONE page. */
    g_bridge_ctx.task = &test_task;
    int brc = gfx_host_fault_install(test_get_jump, &g_bridge_ctx,
                                    region + page, page);
    assert_eq(0, brc);

    /* The test's own setjmp slot — paired with the bridge's
     * __builtin_longjmp.  This is the SYNC POINT where the bridge
     * longjmps back to (longjmp value = 1). */
    volatile int fault_seen = 0;
    if (__builtin_setjmp(g_test_jmp_buf) == 0) {
        gfx_present_rect_req_t req = {
            .pixels = (uint64_t)pixels, .stride = info.stride,
            .reserved = 0, .x = 0, .y = 1, .w = W, .h = 3,
        };
        int rc = do_present_rect(f, &req);
        assert_eq(-EFAULT, rc);
        fault_seen = 1;     /* shouldn't reach here */
    } else {
        /* Longjmp path: the bridge intercepted the row-1 fault. */
        fault_seen = 1;
    }
    assert_eq(1, fault_seen);

    /* Production code restored current->fault_jmp / fault_cleanup /
     * fault_cleanup_arg on every return path. */
    assert_null(test_task.fault_jmp);
    assert_null(test_task.fault_cleanup);
    assert_null(test_task.fault_cleanup_arg);
    /* Lease released. */
    assert_eq(0, g_test_active_writers);

    /* Exactly one write_row (row 0 succeeded, row 1 tripped the fault).
     * Bytes per row = W*4 = 256. */
    assert_eq(1u, g_write_row_calls);
    assert_eq(256u, g_write_row_total_bytes);

    /* Row 0 visible at FB row 1 (view_y=0 + rect_y=1 = 1), cols 0..2.
     * Rows 1..2 still sentinel. */
    for (uint32_t col = 0; col < 3; col++) {
        uint32_t v_expected = 0xCAFE0000u + col;
        assert_eq(v_expected, test_fb_buf[1 * TEST_FB_WIDTH + col]);
    }
    for (uint32_t row = 2; row <= 2; row++) {
        for (uint32_t col = 0; col < 3; col++) {
            assert_eq(0xDEADBEEFu, test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }

    gfx_host_fault_restore();

    /* Follow-up normal present: unprotect, fill row 1+ sources, retry.
     * Proves the bridge's restore puts the host SIGSEGV state back
     * to a state that delivers (and the protected region no longer
     * trips). */
    assert_eq(0, mprotect(region + page, page, PROT_READ | PROT_WRITE));
    /* Fill rect row 1 source (pixels+512 → fb row 2) with the
     * 0xC0DE pattern that matches fb row 2. */
    uint32_t *rowN_pix = (uint32_t *)(pixels + 512);
    for (uint32_t c = 0; c < W; c++) {
        rowN_pix[c] = 0xC0DE0000u + 2 * W + c;
    }
    volatile int fault2 = 0;
    if (__builtin_setjmp(g_test_jmp_buf) == 0) {
        gfx_present_rect_req_t req2 = {
            .pixels = (uint64_t)pixels, .stride = info.stride,
            .reserved = 0, .x = 0, .y = 1, .w = W, .h = 3,
        };
        assert_eq(0, do_present_rect(f, &req2));
        fault2 = 1;
    } else {
        fault2 = 1;
        assert_true(0 && "normal follow-up must not fault");
    }
    assert_eq(1, fault2);
    /* Row 0 still has 0xCAFE from the fault run; row 1 now has 0xC0DE.
     * Row 2 wasn't requested by the rect (h=3 → rows 1,2,3) so the
     * check below tests the actual rect footprint (rows 1, 2, 3 = view
     * rows 1, 2, 3 = fb rows 1, 2, 3).  Wait: rect=(0,1,W,3) means
     * view rows 1, 2, 3 → fb rows 1, 2, 3.  Row 0 was written by the
     * fault run (row 0 of rect = view row 1 = fb row 1).  Rows 1, 2 of
     * the rect are view rows 2, 3 = fb rows 2, 3.
     *   fb row 1 (rect row 0): was 0xCAFE (fault run wrote row 0 of
     *     rect); NOT overwritten because the follow-up only writes
     *     rows 1, 2 of rect.  Wait — the bounce buffer was filled
     *     with new data for rows 1, 2 of the rect (view rows 2+),
     *     but row 0 of the rect (view row 1) wasn't rewritten.
     *     The fb row 1 still has the 0xCAFE pattern from the fault
     *     run.  The follow-up wrote rows 1, 2 of the rect (= view
     *     rows 2, 3 = fb rows 2, 3).  So fb row 2 should have
     *     0xC0DE pattern at row 2, and fb row 3 should have
     *     0xC0DE pattern at row 3. */
    /* Re-fill rect rows 0, 1, 2 sources with the 0xC0DE pattern matching
 * view rows 1, 2, 3 (= fb rows 1, 2, 3). */
    uint32_t *rect_row0_src = (uint32_t *)(pixels + 256);
    for (uint32_t c = 0; c < W; c++) {
        rect_row0_src[c] = 0xC0DE0000u + 1 * W + c;
    }
    uint32_t *rect_row1_src = (uint32_t *)(pixels + 512);
    for (uint32_t c = 0; c < W; c++) {
        rect_row1_src[c] = 0xC0DE0000u + 2 * W + c;
    }
    uint32_t *rect_row2_src = (uint32_t *)(pixels + 768);
    for (uint32_t c = 0; c < W; c++) {
        rect_row2_src[c] = 0xC0DE0000u + 3 * W + c;
    }
    /* Re-issue to overwrite. */
    volatile int fault3 = 0;
    if (__builtin_setjmp(g_test_jmp_buf) == 0) {
        gfx_present_rect_req_t req3 = {
            .pixels = (uint64_t)pixels, .stride = info.stride,
            .reserved = 0, .x = 0, .y = 1, .w = W, .h = 3,
        };
        assert_eq(0, do_present_rect(f, &req3));
        fault3 = 1;
    } else {
        fault3 = 1;
        assert_true(0 && "re-issue must not fault");
    }
    assert_eq(1, fault3);
    for (uint32_t row = 1; row <= 3; row++) {
        for (uint32_t col = 0; col < 3; col++) {
            uint32_t v_expected = 0xC0DE0000u + (row) * W + col;
            assert_eq(v_expected, test_fb_buf[row * TEST_FB_WIDTH + col]);
        }
    }
    assert_eq(0, g_test_active_writers);

    munmap(region, 2 * page);
    file_put(f);

    /* ── Independent fault run #2: different protected region, different
     * view size, proves the bridge re-install path works after a prior
     * install/restore cycle.  Uses a 64x3 view so the geometry stays
     * inside the 64x48 fake FB.  Row 0 source is in writable page 0;
     * row 1 source starts at the first byte of PROT_NONE page 1. */
    reset_test_state();

    file_t *f2 = open_gfx0();
    assert_not_null(f2);
    const uint32_t W2 = TEST_FB_WIDTH, H2 = 3;
    gfx_view_desc_t v2 = { 0, 0, W2, H2 };
    assert_eq(0, do_create_view(f2, &v2));
    gfx_info_t info2;
    assert_eq(0, do_get_info(f2, &info2));
    assert_eq((unsigned)(W2 * 4), info2.stride);
    assert_eq(0, do_acquire(f2));

    uint8_t *region2 = (uint8_t *)mmap(NULL, 2 * page,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS,
                                       -1, 0);
    assert_true(region2 != MAP_FAILED);
    assert_eq(0, mprotect(region2 + page, page, PROT_NONE));

    /* Same layout math as fault run #1: pixels = region2 + page - 512
     * places row 0 source in the writable page and row 1 source at
     * the first byte of PROT_NONE page 1. */
    uint8_t *pixels2 = region2 + page - 512;
    uint32_t *row0_pix2 = (uint32_t *)(pixels2 + 256);
    for (uint32_t c = 0; c < W2; c++) {
        row0_pix2[c] = 0xBABE0000u + c;
    }

    g_bridge_ctx.task = &test_task;
    int brc2 = gfx_host_fault_install(test_get_jump, &g_bridge_ctx,
                                     region2 + page, page);
    assert_eq(0, brc2);

    volatile int fault2_seen = 0;
    if (__builtin_setjmp(g_test_jmp_buf) == 0) {
        gfx_present_rect_req_t req2 = {
            .pixels = (uint64_t)pixels2, .stride = info2.stride,
            .reserved = 0, .x = 0, .y = 1, .w = W2, .h = 2,
        };
        int rc = do_present_rect(f2, &req2);
        assert_eq(-EFAULT, rc);
        fault2_seen = 1;
    } else {
        fault2_seen = 1;
    }
    assert_eq(1, fault2_seen);
    assert_null(test_task.fault_jmp);
    assert_eq(0, g_test_active_writers);

    /* Row 0 of the rect (= fb row 1) was written; row 1 faulted. */
    for (uint32_t c = 0; c < 3; c++) {
        assert_eq(0xBABE0000u + c, test_fb_buf[1 * TEST_FB_WIDTH + c]);
    }
    for (uint32_t c = 0; c < 3; c++) {
        assert_eq(0xDEADBEEFu, test_fb_buf[2 * TEST_FB_WIDTH + c]);
    }

    gfx_host_fault_restore();

    /* Follow-up normal present: unprotect, refill both row sources. */
    assert_eq(0, mprotect(region2 + page, page, PROT_READ | PROT_WRITE));
    uint32_t *r0_pix2 = (uint32_t *)(pixels2 + 256);
    for (uint32_t c = 0; c < W2; c++) {
        r0_pix2[c] = 0xC0DE0000u + 1 * W2 + c;
    }
    uint32_t *r1_pix2 = (uint32_t *)(pixels2 + 512);
    for (uint32_t c = 0; c < W2; c++) {
        r1_pix2[c] = 0xC0DE0000u + 2 * W2 + c;
    }
    volatile int fault2b = 0;
    if (__builtin_setjmp(g_test_jmp_buf) == 0) {
        gfx_present_rect_req_t req2b = {
            .pixels = (uint64_t)pixels2, .stride = info2.stride,
            .reserved = 0, .x = 0, .y = 1, .w = W2, .h = 2,
        };
        assert_eq(0, do_present_rect(f2, &req2b));
        fault2b = 1;
    } else {
        fault2b = 1;
        assert_true(0 && "follow-up #2 must not fault");
    }
    assert_eq(1, fault2b);
    /* Row 0 + row 1 of rect (= fb rows 1 + 2) now have 0xC0DE. */
    for (uint32_t row = 1; row <= 2; row++) {
        for (uint32_t c = 0; c < 3; c++) {
            uint32_t v_expected = 0xC0DE0000u + (row) * W2 + c;
            assert_eq(v_expected, test_fb_buf[row * TEST_FB_WIDTH + c]);
        }
    }
    assert_eq(0, g_test_active_writers);

    munmap(region2, 2 * page);
    file_put(f2);
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
    TEST_ENTRY(test_acquire_unconfigured_returns_einval),
    TEST_ENTRY(test_acquire_stale_mode_returns_estale),
    TEST_ENTRY(test_register_fallback_unconfigured_returns_einval),
    TEST_ENTRY(test_register_fallback_not_fullscreen_returns_einval),
    TEST_ENTRY(test_get_display_state_unconfigured_returns_einval),
    TEST_ENTRY(test_get_display_state_fault_returns_efault),
    TEST_ENTRY(test_replace_fallback_reserved_nonzero_returns_einval),
    TEST_ENTRY(test_replace_fallback_invalid_old_fd_returns_ebadf),
    TEST_ENTRY(test_replace_fallback_fault_returns_efault),
    TEST_ENTRY(test_replace_fallback_non_gfx_fd_returns_einval),
    TEST_ENTRY(test_replace_fallback_null_node_returns_einval),
    TEST_ENTRY(test_present_not_owner_returns_eacces_and_no_writes),
    TEST_ENTRY(test_acquire_and_present_success),
    TEST_ENTRY(test_present_rect_offsets_and_sentinel),
    TEST_ENTRY(test_present_rect_one_pixel_bottom_right_corner),
    TEST_ENTRY(test_present_rect_full_view_dimensions),
    TEST_ENTRY(test_present_rect_invalid_geometry),
    TEST_ENTRY(test_present_rect_errors_and_cleanup),
    TEST_ENTRY(test_present_rect_mid_fault_with_bridge),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
