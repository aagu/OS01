/*
 * test_monitor_file.c — VFS per-file snapshot lifecycle hooks
 *
 * Monitor plan Task 1 / monitor design spec §6
 * (docs/superpowers/specs/2026-10-09-monitor-design.md):
 *   - vfs_ops_t gains optional open_file / read_file / release_file
 *     hooks; file_t gains a separate fs_private (never dev_private);
 *   - open generates the file text once and attaches it to the
 *     file_t; reads return slices of that snapshot, so split reads
 *     never mix two generations of the source data;
 *   - open failure (-ENOMEM / -EOVERFLOW) leaves zero residue: the
 *     hook unmounts its own partial state, the node ref and the
 *     gfx/devfs release counts are untouched;
 *   - release fires exactly once, on the last file_put (dup/fork
 *     share one snapshot), BEFORE the node ref is dropped;
 *   - fd_read keeps its kernel-bounce semantics: a user-copy fault
 *     returns -EFAULT and leaves f->offset unchanged.
 *
 * Build:
 *   - Production kernel/fs/file.c + kernel/fs/devfs.c compiled via
 *     GFX_HOST_CFLAGS against hosttests/mock/gfx_test_runtime.h
 *     (reuses the gfx file-lifecycle harness's production objects).
 *   - This TU defines the stub surface plus the fake snapshot
 *     filesystem callbacks (the procfs counterpart lands in Task 4).
 *   - Tested production symbols: vfs_file_open, vfs_file_read,
 *     vfs_file_release, file_alloc, file_get, file_put, fd_read,
 *     devfs_register_chrdev, devfs_open_node, devfs_release_file.
 */

#include "test_framework.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fs/file.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <lwip/err.h>
#include <lwip/api.h>

/* ── Task + files plumbing (declared in gfx_test_runtime.h) ── */

static task_t   test_task;
static files_t  test_files;
task_t *gfx_test_current = &test_task;
#define current gfx_test_current

uint64_t volatile jiffies;

/* ── Stubs for symbols the production devfs.c + file.c reference ── */

void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }

/* VFS node refcount stubs — balance-tracked so the release tests
 * assert a real number, not a constant. */
static int node_ref_balance;

struct vfs_node *vfs_node_get(struct vfs_node *n)
{
    if (n) { n->refcount++; node_ref_balance++; }
    return n;
}
void vfs_node_put(struct vfs_node *n)
{
    if (!n) return;
    if (n->refcount == 0) {
        assert_true(0 && "vfs_node_put underflow");
        return;
    }
    n->refcount--;
    node_ref_balance--;
    if (n->refcount == 0) free(n);
}

int vfs_mount(const char *p, block_device_t *d,
              const vfs_inode_ops_t *io, const vfs_file_ops_t *fo, void *f)
{ (void)p; (void)d; (void)io; (void)fo; (void)f; return -ENOSYS; }
struct vfs_node *vfs_lookup(const char *p)
{ (void)p; return NULL; }
struct vfs_node *vfs_lookup_from(const char *p, const char *c)
{ (void)p; (void)c; return NULL; }
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{ return node->f_ops->read(node, offset, size, buffer); }

static int injected_copy_error;

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*cb)(void *), void *arg)
{ if (injected_copy_error) { if (cb) cb(arg); return injected_copy_error; }
  memcpy(dst, src, n); return (ssize_t)n; }
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*cb)(void *), void *arg)
{ (void)cb; (void)arg; memcpy(dst, src, n); return (ssize_t)n; }
int strnlen_user(const void *p, size_t m) { (void)p; (void)m; return 0; }
bool syscall_check_user_range(uint64_t a, uint64_t l, bool w)
{ (void)a; (void)l; (void)w; return true; }

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

void wait_queue_init(wait_queue_t *q) { (void)q; }
void wait_queue_sleep(wait_queue_t *q) { (void)q; }
void wait_queue_wake_one(wait_queue_t *q) { (void)q; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }

void task_wake(task_t *t) { (void)t; }
void schedule(void) {}
spinlock_T task_list_lock = { 1 };
gfx_test_task_union_t init_task_union;
list_t *task_list_next(list_t *p) { (void)p; return NULL; }
int signal_pgrp(pid_t t, int s) { (void)t; (void)s; return -1; }

/* ── Fake snapshot filesystem (the procfs side lands in Task 4) ──
 * open_file formats snap_source into a fixed per-open buffer and
 * attaches it to f->fs_private; read_file serves slices of that
 * snapshot; release_file frees it.  snap_fail_mode injects the two
 * open-time failures the spec names. */

#define SNAP_CAP 32

enum snap_fail_mode {
    SNAP_OK = 0,
    SNAP_FAIL_NOMEM,      /* buffer allocation fails            */
    SNAP_FAIL_OVERFLOW,   /* formatting exceeds SNAP_CAP        */
};

static char snap_source[128];
static enum snap_fail_mode snap_fail_mode;
static int snap_attach_count;   /* snapshots actually mounted    */
static int snap_release_count;  /* snapshots actually freed      */

static int snap_open(file_t *f)
{
    if (snap_fail_mode == SNAP_FAIL_NOMEM) return -ENOMEM;
    char *snap = malloc(SNAP_CAP);
    if (!snap) return -ENOMEM;
    int n = snprintf(snap, SNAP_CAP, "%s", snap_source);
    if (n < 0 || n >= SNAP_CAP) {
        /* Formatting overflow: the hook owns its rollback — the
         * half-built snapshot must never stay attached. */
        free(snap);
        return -EOVERFLOW;
    }
    f->fs_private = snap;
    snap_attach_count++;
    return 0;
}

static int64_t snap_read(file_t *f, uint64_t offset, uint64_t size, void *buf)
{
    const char *snap = (const char *)f->fs_private;
    if (!snap) return -EINVAL;
    uint64_t len = strlen(snap);
    if (offset >= len) return 0;            /* stable EOF */
    uint64_t n = len - offset;
    if (n > size) n = size;
    memcpy(buf, snap + offset, n);
    return (int64_t)n;
}

static void snap_release(file_t *f)
{
    /* A failed open left nothing attached: zero residue means the
     * release must be a no-op, not a double free. */
    if (!f->fs_private) return;
    free(f->fs_private);
    f->fs_private = NULL;
    snap_release_count++;
}

static const vfs_file_ops_t snap_fops = {
    .open_file    = snap_open,
    .read_file    = snap_read,
    .release_file = snap_release,
};

/* Heap node (file_free's vfs_node_put frees it when the balance hits
 * zero) + a file carrying exactly one node ref, mirroring what
 * SYS_open's FD_VFS branch assembles. */
static struct vfs_node *snap_node_new(void)
{
    struct vfs_node *n = calloc(1, sizeof(*n));
    n->type = VFS_FILE;
    n->f_ops = &snap_fops;
    n->refcount = 1;
    return n;
}

static file_t *snap_file_new(struct vfs_node *node)
{
    file_t *f = file_alloc();
    if (!f) return NULL;
    vfs_node_get(node);
    f->type = FD_VFS;
    f->node = node;
    f->flags = O_RDONLY;
    return f;
}

/* ── Fake gfx character device (devfs protocol must stay intact) ── */

static int gfx_open_count;
static int gfx_release_count;

static int fake_gfx_open(const char *name, file_t **out_file)
{
    (void)name;
    if (!out_file) return -EINVAL;
    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    f->type = FD_NONE;   /* devfs_open_node stamps FD_DEV + node */
    f->dev_private = calloc(1, sizeof(uint64_t));
    gfx_open_count++;
    *out_file = f;
    return 0;
}

static void fake_gfx_release(file_t *f)
{
    free(f->dev_private);
    f->dev_private = NULL;
    gfx_release_count++;
}

static int registered_count;

static int register_fake_gfx(void)
{
    static const struct devfs_ops ops = {
        .open         = fake_gfx_open,
        .release_file = fake_gfx_release,
    };
    int idx = registered_count++;
    int rc = devfs_register_chrdev("monitor_gfx", NULL, &ops);
    if (rc < 0) { registered_count--; return rc; }
    return idx;
}

static struct vfs_node *gfx_node_with_idx(int idx)
{
    struct vfs_node *n = calloc(1, sizeof(*n));
    n->type = VFS_CHRDEV;
    n->fs_data = (void *)(uintptr_t)idx;
    n->refcount = 1;
    return n;
}

static void reset_test_state(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
    node_ref_balance = 0;
    jiffies = 0;
    snap_source[0] = '\0';
    snap_fail_mode = SNAP_OK;
    snap_attach_count = 0;
    snap_release_count = 0;
    gfx_open_count = 0;
    gfx_release_count = 0;
    injected_copy_error = 0;
    /* registered_count and the production devices[] table persist
     * across sub-tests (devfs cannot be re-inited); each test
     * registers its own fresh device entry. */
}

/* ── Tests ──────────────────────────────────────────── */

/* Split reads must serve slices of the ONE open-time snapshot: when
 * the source data changes between two reads, the concatenated bytes
 * are still the first generation's text (spec §6.4). */
TEST_FUNC(snapshot_split_read_is_stable) {
    reset_test_state();
    struct vfs_node *node = snap_node_new();
    file_t *f = snap_file_new(node);
    assert_not_null(f);

    strcpy(snap_source, "Version: 1\nSampleNs: 100");
    char first_generation[sizeof(snap_source)];
    strcpy(first_generation, snap_source);

    /* fs_private starts NULL and open mounts exactly one snapshot. */
    assert_true(f->fs_private == NULL);
    assert_eq(0, vfs_file_open(f));
    assert_not_null(f->fs_private);
    assert_eq(1, snap_attach_count);
    /* The separate fs_private must not leak into the device slot. */
    assert_true(f->dev_private == NULL);

    /* Read in two slices through the production fd_read bounce path,
     * mutating the source between them. */
    char joined[64];
    assert_eq((int64_t)12, fd_read(f, joined, 12));
    strcpy(snap_source, "MUTATED AFTER OPEN — MUST NOT APPEAR");
    assert_eq((int64_t)12, fd_read(f, joined + 12, 12));
    assert_eq((uint64_t)24, f->offset);
    joined[24] = '\0';
    assert_str_eq(first_generation, joined);

    /* Reads past the stable snapshot length hit EOF, not an error. */
    char eof_buf[4];
    assert_eq((int64_t)0, fd_read(f, eof_buf, sizeof(eof_buf)));
    assert_eq((uint64_t)24, f->offset);

    vfs_file_release(f);
    assert_eq(1, snap_release_count);
    file_put(f);                       /* frees node ref + file */
    assert_eq(0, node_ref_balance);
}

/* dup()/fork() bump the file refcount without re-opening: the
 * snapshot is released exactly once, on the drop-to-zero transition
 * (spec §6.5), and never while a reference is still held. */
TEST_FUNC(duplicate_release_once) {
    reset_test_state();
    struct vfs_node *node = snap_node_new();
    file_t *f = snap_file_new(node);
    assert_not_null(f);

    strcpy(snap_source, "cpu0: busy=1");
    assert_eq(0, vfs_file_open(f));
    assert_eq(1, snap_attach_count);

    file_get(f);   /* dup */
    file_get(f);   /* fork */

    file_put(f);
    file_put(f);
    assert_eq(0, snap_release_count);  /* still one live reference */

    file_put(f);   /* final drop → file_free → vfs_file_release */
    assert_eq(1, snap_release_count);
    assert_eq(0, node_ref_balance);    /* node ref dropped after release */
    /* f was freed by file_free — do not touch it again. */
}

/* ENOMEM and EOVERFLOW at open time leave zero residue: nothing is
 * attached, nothing is freed twice, the node ref balance is intact,
 * and the devfs/gfx release protocol is untouched. */
TEST_FUNC(failed_open_cleanup) {
    reset_test_state();
    int gfx_idx = register_fake_gfx();
    assert_true(gfx_idx >= 0);

    /* Keep a live device file open so the "unchanged" claims are
     * meaningful (the gfx release must NOT fire for snapshot
     * failures). */
    struct vfs_node *gfx_node = gfx_node_with_idx(gfx_idx);
    file_t *gfx_f = NULL;
    assert_eq(0, devfs_open_node(gfx_node, "/dev/monitor_gfx", O_RDWR, &gfx_f));
    assert_not_null(gfx_f);
    assert_not_null(gfx_f->dev_private);
    assert_eq(1, gfx_open_count);
    assert_eq(0, gfx_release_count);
    /* The open device file holds one node ref for as long as it lives;
     * mid-test balance assertions compare against this baseline. */
    int balance_with_gfx = node_ref_balance;

    /* -ENOMEM: nothing attached, release is a no-op. */
    struct vfs_node *n1 = snap_node_new();
    file_t *f1 = snap_file_new(n1);
    assert_not_null(f1);
    strcpy(snap_source, "Version: 1");
    snap_fail_mode = SNAP_FAIL_NOMEM;
    assert_eq(-ENOMEM, vfs_file_open(f1));
    assert_true(f1->fs_private == NULL);
    assert_eq(0, snap_attach_count);
    file_put(f1);
    assert_eq(0, snap_release_count);
    assert_eq(balance_with_gfx, node_ref_balance);

    /* -EOVERFLOW: the hook rolled back its own half-built snapshot. */
    struct vfs_node *n2 = snap_node_new();
    file_t *f2 = snap_file_new(n2);
    assert_not_null(f2);
    strcpy(snap_source,
           "this line is deliberately much longer than SNAP_CAP bytes");
    snap_fail_mode = SNAP_OK;   /* overflow comes from the length check */
    assert_eq(-EOVERFLOW, vfs_file_open(f2));
    assert_true(f2->fs_private == NULL);
    assert_eq(0, snap_attach_count);
    file_put(f2);
    assert_eq(0, snap_release_count);
    assert_eq(balance_with_gfx, node_ref_balance);

    /* gfx/devfs release counts unchanged throughout. */
    assert_eq(0, gfx_release_count);
    file_put(gfx_f);
    assert_eq(1, gfx_release_count);   /* protocol still fires once */
    assert_eq(0, node_ref_balance);
}

/* A user-buffer fault inside fd_read's kernel bounce returns -EFAULT
 * and leaves f->offset untouched; the retry after clearing the fault
 * reads on from the preserved offset. */
TEST_FUNC(user_copy_fault_preserves_offset) {
    reset_test_state();
    struct vfs_node *node = snap_node_new();
    file_t *f = snap_file_new(node);
    assert_not_null(f);

    strcpy(snap_source, "0123456789ABCDEF");
    assert_eq(0, vfs_file_open(f));

    char buf[8];
    assert_eq((int64_t)4, fd_read(f, buf, 4));
    assert_eq(0, memcmp(buf, "0123", 4));
    uint64_t offset_before_fault = f->offset;

    injected_copy_error = -EFAULT;
    assert_eq(-EFAULT, fd_read(f, buf, sizeof(buf)));
    assert_eq(f->offset, offset_before_fault);

    injected_copy_error = 0;
    assert_eq((int64_t)8, fd_read(f, buf, sizeof(buf)));
    assert_eq(0, memcmp(buf, "456789AB", 8));
    assert_eq(offset_before_fault + 8, f->offset);

    vfs_file_release(f);
    assert_eq(1, snap_release_count);
    file_put(f);
    assert_eq(0, node_ref_balance);
}

TEST_LIST_BEGIN
    TEST_ENTRY(snapshot_split_read_is_stable),
    TEST_ENTRY(duplicate_release_once),
    TEST_ENTRY(failed_open_cleanup),
    TEST_ENTRY(user_copy_fault_preserves_offset),
TEST_LIST_END

int main(void) {
    for (int i = 0; i < __test_table_size; i++) __test_table[i].fn();
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed != 0;
}
