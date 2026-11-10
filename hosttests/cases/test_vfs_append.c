/*
 * test_vfs_append.c — libc-completion Task T6: kernel atomic append.
 *
 * Drives the REAL production chain fd_write() → vfs_append() →
 * {tmpfs,ext2,fat}_vfs_append() with a real open-file description,
 * on real filesystem images inside a host RAM disk (ext2 and FAT32
 * are mounted from a byte-array block device exactly like the real
 * drivers see them).
 *
 * What is proven here (per the plan's T6 checklist):
 *   1. Two INDEPENDENT open-file descriptions appending to the same
 *      underlying file must not overwrite each other: the second
 *      appender's bytes land at the file's REAL end (the pre-T6
 *      fd_write wrote at the caller's own stale offset and clobbered
 *      the start — that behaviour fails this test, which is the RED
 *      evidence).
 *   2. A SHARED file_t whose offset went stale (another appender
 *      extended the file behind it) must come out of fd_write with
 *      offset == last commit end — proves fd_write does NOT re-assign
 *      the offset after the hook already did it ("A returns after B"
 *      clobber protection; the true SMP interleaving of that scenario
 *      is covered by the systest append case).
 *   3. size > UACCESS_BOUNCE_SIZE is a LEGAL short write of exactly
 *      one bounce, committed atomically; the rest follows in a second
 *      call.
 *   4. A failing user copy (EFAULT) and a failing bounce allocation
 *      (ENOMEM) commit NOTHING — content, size and offset are all
 *      untouched.
 *   5. truncate + append share the filesystem's serialization: after a
 *      truncate to 0 the next append lands at 0.
 *   6. The VFS-level dispatch errno contract: a filesystem with no
 *      write op is EROFS, a writable filesystem without an append hook
 *      is EOPNOTSUPP, and a device file opened O_APPEND stays on the
 *      plain write path (O_APPEND does not make devices appendable).
 *
 * Scenarios 1–4 run on tmpfs, ext2 AND FAT32 where the plan requires
 * it (per-FS EOF selection), the rest on tmpfs — the dispatch and
 * bounce logic above the FS hook is filesystem-independent.
 */

#include "test_framework.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <fs/vfs.h>
#include <fs/file.h>
#include <block/blockdev.h>
#include <memory/uaccess.h>      /* UACCESS_BOUNCE_SIZE */

#include "vfs_ram_disk.h"        /* RAM disk + ext2/FAT32 formatters */
#include "vfs_append_runtime.h"  /* fault injection + file.c stubs  */

/* ── Per-binary runtime surface (see test_vfs_fs_registry.c) ── */

static task_t s_test_task = {
    .pid = 1,
    .addr_limit = 0x00007fffffffffffULL,
};
task_t *vfs_test_current = &s_test_task;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

/* The REAL block-device dispatch: ext2/fat read and write the RAM
 * disk through this wrapper (the registry test stubs it -EIO because
 * it performs no IO; this test performs nothing else). */
int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->ops || !dev->ops->read) return -EIO;
    return dev->ops->read(dev, lba, count, buf);
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->ops || !dev->ops->write) return -EIO;
    return dev->ops->write(dev, lba, count, buf);
}

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

/* vfs_init() registers procfs too; the real procfs.c is not linked in
 * this binary (same override as test_vfs_fs_registry.c). */
static const vfs_inode_ops_t s_dummy_iops = {0};
static const vfs_file_ops_t  s_dummy_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_dummy_iops;
    mp->f_ops = &s_dummy_fops;
    mp->root->i_ops = &s_dummy_iops;
    mp->root->f_ops = &s_dummy_fops;
    return 0;
}
static vfs_fs_type_t s_host_procfs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = host_procfs_mount,
};
int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }

/* ── Open-file helpers (mirrors the sys_open FD_VFS path) ───── */

static file_t *open_vfs_file(const char *path, int flags)
{
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return NULL;
    file_t *f = file_alloc();
    if (!f) { vfs_node_put(node); return NULL; }
    f->type = FD_VFS;
    f->node = node;          /* f owns the lookup ref */
    f->flags = flags;
    f->offset = 0;
    return f;
}

static void close_vfs_file(file_t *f)
{
    if (f) file_free(f);
}

/* Create the file and seed it with `seed` through the PLAIN write
 * path (no O_APPEND) — the pre-existing content every append must
 * preserve. */
static int seed_file(const char *path, const char *seed)
{
    vfs_node_t *node = NULL;
    int rc = vfs_create_file(path, "/", &node);
    if (rc != 0) return rc;
    vfs_node_put(node);

    if (*seed == '\0') return 0;      /* fd_write rejects size 0; nothing to seed */
    file_t *f = open_vfs_file(path, O_WRONLY);
    if (!f) return -ENOENT;
    int64_t n = fd_write(f, seed, strlen(seed));
    close_vfs_file(f);
    return (n == (int64_t)strlen(seed)) ? 0 : -EIO;
}

/* Read the whole file back through the node (independent of any
 * file_t state). */
static int read_file_node(const char *path, char *out, uint64_t cap, uint64_t *out_size)
{
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    int n = vfs_read(node, 0, cap, out);
    vfs_node_put(node);
    if (n < 0) return n;
    *out_size = (uint64_t)n;
    return 0;
}

static uint64_t file_size(const char *path)
{
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return (uint64_t)-1;
    uint64_t s = node->size;
    vfs_node_put(node);
    return s;
}

/* Two independent opens, A then B: every append must land at the
 * file's real end (parameterised over the three writable FS). */
static void append_two_independent_opens(const char *path)
{
    char buf[256];
    uint64_t sz = 0;

    assert_eq(0, seed_file(path, "AAAA"));

    file_t *fa = open_vfs_file(path, O_WRONLY | O_APPEND);
    file_t *fb = open_vfs_file(path, O_WRONLY | O_APPEND);
    assert_not_null(fa);
    assert_not_null(fb);

    /* A appends token 1: lands at EOF 4, commits contiguously. */
    assert_eq(4, (int)fd_write(fa, "1111", 4));
    assert_eq(8, (uint64_t)file_size(path));
    assert_eq(8, (uint64_t)fa->offset);          /* hook moved it to the commit end */
    assert_eq(0, read_file_node(path, buf, sizeof(buf), &sz));
    assert_eq(8, sz);
    assert_mem_eq("AAAA1111", buf, 8);           /* initial content preserved, no overlap */

    /* B appends token 2: B's own offset is stale (0) — the bytes must
     * land at the REAL end 8, not at 0 (the pre-T6 defect overwrote
     * the start here). */
    assert_eq(4, (int)fd_write(fb, "2222", 4));
    assert_eq(12, (uint64_t)file_size(path));
    assert_eq(12, (uint64_t)fb->offset);
    assert_eq(8, (uint64_t)fa->offset);          /* independent descriptions, no cross-talk */
    assert_eq(0, read_file_node(path, buf, sizeof(buf), &sz));
    assert_eq(12, sz);
    assert_mem_eq("AAAA11112222", buf, 12);      /* every syscall's token contiguous */

    close_vfs_file(fa);
    close_vfs_file(fb);
}

/* Shared open-file description with a STALE offset: another appender
 * extended the file after `f` was opened; f's next O_APPEND write must
 * return with offset == last commit end (6), NOT old_offset + n (3)
 * and NOT its stale 0 — fd_write must not re-assign the offset the
 * hook already set. */
static void append_shared_stale_offset(const char *path)
{
    char buf[256];
    uint64_t sz = 0;

    assert_eq(0, seed_file(path, ""));

    file_t *f  = open_vfs_file(path, O_RDWR | O_APPEND);   /* offset 0, will go stale */
    file_t *fg = open_vfs_file(path, O_WRONLY | O_APPEND);
    assert_not_null(f);
    assert_not_null(fg);

    /* The other appender extends the file behind f's back. */
    assert_eq(3, (int)fd_write(fg, "111", 3));
    assert_eq(3, (uint64_t)file_size(path));
    assert_eq(0, (uint64_t)f->offset);           /* f's offset is now stale */

    /* f appends: the hook commits at EOF 3 and sets f->offset = 6.
     * A plain-path return would wrongly compute 0 + 3 = 3. */
    assert_eq(3, (int)fd_write(f, "222", 3));
    assert_eq(6, (uint64_t)file_size(path));
    assert_eq(6, (uint64_t)f->offset);           /* last commit end, never a stale value */
    assert_eq(0, read_file_node(path, buf, sizeof(buf), &sz));
    assert_mem_eq("111222", buf, 6);

    close_vfs_file(f);
    close_vfs_file(fg);
}

/* ── Synthetic nodes for the dispatch-contract cases ────────── */

static int g_recording_calls = 0;
static int recording_write(struct vfs_node *node, uint64_t offset,
                           uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    g_recording_calls++;
    return (int)size;
}
static const vfs_file_ops_t s_appendless_fops = {
    .write = recording_write,               /* writable, no append hook */
};
static const vfs_file_ops_t s_readonly_fops = {
    /* no write, no append */
};

/* ── tmpfs section ──────────────────────────────────────────── */

TEST_FUNC(test_append_two_independent_opens_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    append_two_independent_opens("/a.txt");
}

TEST_FUNC(test_append_shared_stale_offset_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    append_shared_stale_offset("/a.txt");
}

TEST_FUNC(test_append_bounce_short_write_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    static uint8_t big[UACCESS_BOUNCE_SIZE + 64];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i & 0x7f);

    assert_eq(0, seed_file("/a.txt", "AAAA"));
    file_t *f = open_vfs_file("/a.txt", O_WRONLY | O_APPEND);
    assert_not_null(f);

    /* A request larger than one bounce is a LEGAL short write of
     * exactly one bounce: the hook is called once and the offset ends
     * at old EOF + committed bytes. */
    int64_t n = fd_write(f, big, sizeof(big));
    assert_eq((int64_t)UACCESS_BOUNCE_SIZE, (int64_t)n);
    assert_eq(4 + (uint64_t)UACCESS_BOUNCE_SIZE, (uint64_t)f->offset);
    assert_eq(4 + (uint64_t)UACCESS_BOUNCE_SIZE, (uint64_t)file_size("/a.txt"));

    /* The committed bytes are the FIRST bounce of the request, in
     * order. */
    static uint8_t tail[64];
    vfs_node_t *node = vfs_lookup("/a.txt");
    assert_not_null(node);
    assert_eq((int)sizeof(tail),
              vfs_read(node, 4 + (uint64_t)UACCESS_BOUNCE_SIZE - sizeof(tail),
                       sizeof(tail), tail));
    vfs_node_put(node);
    assert_mem_eq(big + UACCESS_BOUNCE_SIZE - sizeof(tail), tail, sizeof(tail));

    /* The rest follows in a second call. */
    assert_eq((int)sizeof(tail),
              (int)fd_write(f, big + UACCESS_BOUNCE_SIZE, sizeof(tail)));
    assert_eq(4 + (uint64_t)UACCESS_BOUNCE_SIZE + sizeof(tail),
              (uint64_t)f->offset);
    assert_eq(4 + (uint64_t)UACCESS_BOUNCE_SIZE + sizeof(tail),
              (uint64_t)file_size("/a.txt"));

    close_vfs_file(f);
}

TEST_FUNC(test_append_efault_commits_nothing_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    char buf[64];
    uint64_t sz = 0;
    assert_eq(0, seed_file("/a.txt", "AAAA"));

    file_t *f = open_vfs_file("/a.txt", O_WRONLY | O_APPEND);
    assert_not_null(f);

    /* The user copy fails BEFORE the filesystem is touched: nothing is
     * committed, the error propagates, the offset is untouched. */
    vfs_append_user_copy_fault = 1;
    int64_t rc = fd_write(f, "1111", 4);
    vfs_append_user_copy_fault = 0;
    assert_eq(-EFAULT, (int)rc);
    assert_eq(4, (uint64_t)file_size("/a.txt"));
    assert_eq(0, (uint64_t)f->offset);
    assert_eq(0, read_file_node("/a.txt", buf, sizeof(buf), &sz));
    assert_mem_eq("AAAA", buf, 4);

    /* The same write succeeds once the fault is disarmed. */
    assert_eq(4, (int)fd_write(f, "1111", 4));
    assert_eq(8, (uint64_t)f->offset);

    close_vfs_file(f);
}

TEST_FUNC(test_append_enomem_commits_nothing_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    char buf[64];
    uint64_t sz = 0;
    assert_eq(0, seed_file("/a.txt", "AAAA"));

    file_t *f = open_vfs_file("/a.txt", O_WRONLY | O_APPEND);
    assert_not_null(f);

    /* The bounce allocation fails before anything else happens. */
    vfs_append_bounce_alloc_fail = 1;
    int64_t rc = fd_write(f, "1111", 4);
    vfs_append_bounce_alloc_fail = 0;
    assert_eq(-ENOMEM, (int)rc);
    assert_eq(4, (uint64_t)file_size("/a.txt"));
    assert_eq(0, (uint64_t)f->offset);
    assert_eq(0, read_file_node("/a.txt", buf, sizeof(buf), &sz));
    assert_mem_eq("AAAA", buf, 4);

    assert_eq(4, (int)fd_write(f, "1111", 4));
    assert_eq(8, (uint64_t)f->offset);

    close_vfs_file(f);
}

TEST_FUNC(test_append_after_truncate_lands_at_zero_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    char buf[64];
    uint64_t sz = 0;
    assert_eq(0, seed_file("/a.txt", "AAAA"));

    vfs_node_t *node = vfs_lookup("/a.txt");
    assert_not_null(node);
    assert_eq(0, vfs_truncate(node, 0));
    assert_eq(0, (uint64_t)node->size);
    vfs_node_put(node);

    /* Truncate and append serialize on the same mount mutex: the
     * append observes the truncated EOF (0), not a stale size. */
    file_t *f = open_vfs_file("/a.txt", O_WRONLY | O_APPEND);
    assert_not_null(f);
    assert_eq(2, (int)fd_write(f, "XY", 2));
    assert_eq(2, (uint64_t)f->offset);
    assert_eq(0, read_file_node("/a.txt", buf, sizeof(buf), &sz));
    assert_mem_eq("XY", buf, 2);

    close_vfs_file(f);
}

TEST_FUNC(test_append_dispatch_erofs_eopnotsupp_and_dev) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    /* Writable FS without an append hook: fd_write(O_APPEND) fails
     * with EOPNOTSUPP and the plain write is NOT performed. */
    static vfs_node_t plain_node;
    memset(&plain_node, 0, sizeof(plain_node));
    plain_node.f_ops = &s_appendless_fops;

    static file_t plain_file;
    memset(&plain_file, 0, sizeof(plain_file));
    plain_file.type = FD_VFS;
    plain_file.node = &plain_node;
    plain_file.flags = O_WRONLY | O_APPEND;
    plain_file.offset = 0;
    g_recording_calls = 0;
    assert_eq(-EOPNOTSUPP, (int)fd_write(&plain_file, "zz", 2));
    assert_eq(0, g_recording_calls);
    assert_eq(0, (uint64_t)plain_file.offset);

    /* A read-only filesystem (no write op): EROFS, reported by the
     * VFS dispatch itself. */
    static vfs_node_t ro_node;
    memset(&ro_node, 0, sizeof(ro_node));
    ro_node.f_ops = &s_readonly_fops;

    static file_t ro_file;
    memset(&ro_file, 0, sizeof(ro_file));
    ro_file.type = FD_VFS;
    ro_file.node = &ro_node;
    ro_file.flags = O_WRONLY | O_APPEND;
    assert_eq(-EROFS, (int)vfs_append(&ro_file, 2, "zz"));
    /* fd_write's own write-op guard answers -1 BEFORE the append
     * dispatch is reached; EROFS is vfs_append's contract. */
    assert_eq(-1, (int)fd_write(&ro_file, "zz", 2));

    /* A DEVICE file opened O_APPEND stays on the plain write path —
     * O_APPEND must not reroute devices into the VFS append. */
    static vfs_node_t dev_node;
    memset(&dev_node, 0, sizeof(dev_node));
    dev_node.f_ops = &s_appendless_fops;

    static file_t dev_file;
    memset(&dev_file, 0, sizeof(dev_file));
    dev_file.type = FD_DEV;
    dev_file.node = &dev_node;
    dev_file.flags = O_WRONLY | O_APPEND;
    dev_file.offset = 0;
    g_recording_calls = 0;
    assert_eq(2, (int)fd_write(&dev_file, "zz", 2));
    assert_eq(1, g_recording_calls);             /* plain write happened */
    assert_eq(2, (uint64_t)dev_file.offset);     /* plain path advanced it */
}

/* ── ext2 section ───────────────────────────────────────────── */

TEST_FUNC(test_append_two_independent_opens_ext2) {
    vfs_init();
    format_mock_ext2_disk();
    assert_eq(0, vfs_mount_fs("ext2", "/", &s_ram_block_dev, NULL));
    append_two_independent_opens("/a.txt");
    append_shared_stale_offset("/b.txt");
}

/* ── FAT32 section ──────────────────────────────────────────── */

TEST_FUNC(test_append_two_independent_opens_fat) {
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));
    append_two_independent_opens("/a.txt");
    append_shared_stale_offset("/b.txt");
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_append_two_independent_opens_tmpfs),
    TEST_ENTRY(test_append_shared_stale_offset_tmpfs),
    TEST_ENTRY(test_append_bounce_short_write_tmpfs),
    TEST_ENTRY(test_append_efault_commits_nothing_tmpfs),
    TEST_ENTRY(test_append_enomem_commits_nothing_tmpfs),
    TEST_ENTRY(test_append_after_truncate_lands_at_zero_tmpfs),
    TEST_ENTRY(test_append_dispatch_erofs_eopnotsupp_and_dev),
    TEST_ENTRY(test_append_two_independent_opens_ext2),
    TEST_ENTRY(test_append_two_independent_opens_fat),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
