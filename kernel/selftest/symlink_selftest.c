// kernel/selftest/symlink_selftest.c — kernel selftests for symlink support.
//
// Covers the resolver and ext2 rollback paths that userspace cannot reach:
//
//   symlink_resolver       — drives the production vfs_lookup_at() against a
//                            deterministic in-memory fake filesystem mounted
//                            through the production vfs_mount() API.  Proves
//                            mid-path NOFOLLOW still follows (POSIX), and the
//                            exact -ENOTDIR / -ELOOP / -ENAMETOOLONG /
//                            -EOPNOTSUPP / -EIO results.
//
//   symlink_ext2_rollback  — drives ext2_vfs_symlink()/ext2_vfs_unlink() via
//                            the real root mount's ops, using the
//                            ext2_symlink_test_set_fault() injection hooks to
//                            force each I/O step to fail, and asserts that a
//                            failed create leaves no dirent and restores every
//                            allocation.  Also asserts type-aware unlink: a
//                            fast symlink frees no block, a long symlink frees
//                            exactly one.
//
// This file is compiled in ordinary kernel builds too (kernel/Makefile globs
// test/*.c), so the whole body is gated on OS01_SELFTEST and the exported
// entry points degrade to trivial stubs elsewhere.  Registered explicitly in
// selftest_run_all(); the runner does not scan .selftest_table, so SELFTEST()
// is deliberately not used.

#include <kernel/selftest.h>
#include <kernel/printk.h>
#include <fs/vfs.h>
#include <fs/ext2.h>
#include <uapi/stat.h>       // AT_FDCWD
#include <string.h>
#include <errno.h>

int symlink_selftest_resolver(void);
int symlink_selftest_ext2_rollback(void);

#if defined(OS01_SELFTEST)

// ══════════════════════════════════════════════════════════
// Part 1 — deterministic in-memory filesystem for the resolver
// ══════════════════════════════════════════════════════════
//
// __vfs_lookup_raw() builds each child node from the parent's readdir entry,
// inheriting the parent's ops and taking entry.ino as the child's fs_data.
// So a node's identity here is simply its index into fake_ents[], and index 0
// is the mount root (vfs_mount sets the root node's fs_data to NULL).

typedef struct {
    const char *name;
    uint8_t     type;
    int         parent;
    const char *target;   // symlinks only
} fake_ent_t;

// NOTE: most targets below are absolute.  FAKE_RELLINK deliberately uses a
// relative target ("dir/target") so the sub-mount relative-resolution
// selftest at the bottom of symlink_selftest_resolver() exercises the
// __vfs_lookup_raw() fix that seeds consumed with mp->path — without that
// fix, splice_symlink_path() splices the relative target against "/" and
// resolves in the root mount instead of inside the symlink-selftest mount.

// An absolute target of exactly 254 bytes: short enough to clear the readlink
// length guard (tlen >= VFS_NAME_MAX - 1), long enough that splicing it onto a
// "/x" suffix overflows VFS_NAME_MAX (254 + 2 + 1 > 256).  So the failure must
// come from splice_symlink_path(), not from the readlink guard.
static const char fake_big_target[] =
    "/symlink-selftest/"
    "0123456789012345678901234567890123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789012345678901234567890123456789"
    "0123456789012345678901234567890123456789012345678901234567890123456789"
    "01234567890123456789012345";  // 18 + 236 = 254 chars

enum {
    FAKE_ROOT = 0,
    FAKE_DIR,        // /dir
    FAKE_FILE,       // /file          (non-dir, for the ENOTDIR case)
    FAKE_TARGET,     // /dir/target
    FAKE_MIDLINK,    // /midlink    -> /symlink-selftest/dir
    FAKE_BIG,        // /big        -> 254-char absolute target
    FAKE_BADDIR,     // /baddir     (readdir fails, for the EIO case)
    FAKE_RELLINK,    // /rellink    -> "dir/target"  (RELATIVE target)
    FAKE_L0,         // /l0 .. /l9  chain of 10 symlinks (MAXSYMLINKS is 8)
};

static const fake_ent_t fake_ents[] = {
    [FAKE_ROOT]    = { "/",       VFS_DIR,     -1,        NULL },
    [FAKE_DIR]     = { "dir",     VFS_DIR,     FAKE_ROOT, NULL },
    [FAKE_FILE]    = { "file",    VFS_FILE,    FAKE_ROOT, NULL },
    [FAKE_TARGET]  = { "target",  VFS_FILE,    FAKE_DIR,  NULL },
    [FAKE_MIDLINK] = { "midlink", VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/dir" },
    [FAKE_BIG]     = { "big",     VFS_SYMLINK, FAKE_ROOT, fake_big_target },
    [FAKE_BADDIR]  = { "baddir",  VFS_DIR,     FAKE_ROOT, NULL },
    [FAKE_RELLINK] = { "rellink", VFS_SYMLINK, FAKE_ROOT, "dir/target" },
    [FAKE_L0 + 0]  = { "l0",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l1" },
    [FAKE_L0 + 1]  = { "l1",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l2" },
    [FAKE_L0 + 2]  = { "l2",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l3" },
    [FAKE_L0 + 3]  = { "l3",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l4" },
    [FAKE_L0 + 4]  = { "l4",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l5" },
    [FAKE_L0 + 5]  = { "l5",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l6" },
    [FAKE_L0 + 6]  = { "l6",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l7" },
    [FAKE_L0 + 7]  = { "l7",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l8" },
    [FAKE_L0 + 8]  = { "l8",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/l9" },
    [FAKE_L0 + 9]  = { "l9",      VFS_SYMLINK, FAKE_ROOT, "/symlink-selftest/file" },
};

#define FAKE_COUNT ((int)(sizeof(fake_ents) / sizeof(fake_ents[0])))

static int fake_self_index(struct vfs_node *node)
{
    return (int)(uintptr_t)node->fs_data;   // root node's fs_data is NULL → 0
}

static int fake_readdir(struct vfs_node *node, uint64_t index,
                        struct vfs_dirent *entry)
{
    int self = fake_self_index(node);
    if (self < 0 || self >= FAKE_COUNT) return -1;

    // Case 37: a directory whose readdir fails must surface as -EIO from the
    // walker, not as a spurious -ENOENT.
    if (self == FAKE_BADDIR) return -1;

    uint64_t seen = 0;
    for (int i = 1; i < FAKE_COUNT; i++) {
        if (fake_ents[i].parent != self) continue;
        if (seen == index) {
            size_t nlen = strlen(fake_ents[i].name);
            memcpy(entry->name, fake_ents[i].name, nlen + 1);
            entry->type = fake_ents[i].type;
            entry->size = 0;
            entry->ino  = (uint64_t)i;
            return 0;
        }
        seen++;
    }
    entry->name[0] = '\0';   // end of directory
    return 0;
}

static int fake_readlink(struct vfs_node *node, char *buf, size_t size)
{
    int self = fake_self_index(node);
    if (self <= 0 || self >= FAKE_COUNT) return -EINVAL;
    const char *target = fake_ents[self].target;
    if (!target) return -EINVAL;

    size_t tlen = strlen(target);
    if (tlen > size) tlen = size;
    memcpy(buf, target, tlen);
    return (int)tlen;
}

static vfs_ops_t fake_ops = {
    .readdir  = fake_readdir,
    .readlink = fake_readlink,
};

// Same tree, but the ops table has no readlink — case 32 (-EOPNOTSUPP).
// readlink is per-ops, and children inherit their parent's ops, so a second
// mount is the only way to present a VFS_SYMLINK node with .readlink == NULL.
static vfs_ops_t fake_ops_no_readlink = {
    .readdir  = fake_readdir,
    .readlink = NULL,
};

#define FAKE_MNT      "/symlink-selftest"
#define FAKE_MNT_NORL "/symlink-selftest-nolink"

static int fake_mounted = 0;

static int fake_mount_once(void)
{
    if (fake_mounted) return 0;
    if (vfs_mount(FAKE_MNT, NULL, &fake_ops, NULL) != 0) return -1;
    if (vfs_mount(FAKE_MNT_NORL, NULL, &fake_ops_no_readlink, NULL) != 0) return -1;
    fake_mounted = 1;
    return 0;
}

// Expect vfs_lookup_at to fail with exactly `want` (a negative errno).
static int expect_err(const char *path, lookup_flags_t flags, int want)
{
    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path, flags, &node);
    if (rc != want) {
        serial_printk("[selftest] symlink_resolver: %s -> %d, expected %d\n",
                      path, rc, want);
        if (rc == 0 && node) vfs_node_put(node);
        return -1;
    }
    if (node) {
        serial_printk("[selftest] symlink_resolver: %s failed but returned a node\n",
                      path);
        vfs_node_put(node);
        return -1;
    }
    return 0;
}

// Expect vfs_lookup_at to succeed and land on a node of type `want_type`.
static int expect_type(const char *path, lookup_flags_t flags, uint8_t want_type)
{
    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path, flags, &node);
    if (rc != 0 || !node) {
        serial_printk("[selftest] symlink_resolver: %s -> %d, expected success\n",
                      path, rc);
        return -1;
    }
    int ok = (node->type == want_type);
    if (!ok) {
        serial_printk("[selftest] symlink_resolver: %s type=%d, expected %d\n",
                      path, (int)node->type, (int)want_type);
    }
    vfs_node_put(node);
    return ok ? 0 : -1;
}

int symlink_selftest_resolver(void)
{
    if (fake_mount_once() != 0) {
        serial_printk("[selftest] symlink_resolver: vfs_mount of fake tree failed\n");
        return -1;
    }

    // POSIX: NOFOLLOW blocks only the LAST component.  A mid-path symlink is
    // still followed, so this must resolve all the way to the regular file.
    if (expect_type(FAKE_MNT "/midlink/target", LOOKUP_NOFOLLOW, VFS_FILE) != 0)
        return -1;

    // ...while NOFOLLOW on the last component returns the symlink itself.
    if (expect_type(FAKE_MNT "/midlink", LOOKUP_NOFOLLOW, VFS_SYMLINK) != 0)
        return -1;

    // ...and FOLLOW on the same path resolves through to the directory.
    if (expect_type(FAKE_MNT "/midlink", LOOKUP_FOLLOW, VFS_DIR) != 0)
        return -1;

    // Case 36: a non-directory in the middle of the path.
    if (expect_err(FAKE_MNT "/file/x", LOOKUP_FOLLOW, -ENOTDIR) != 0)
        return -1;

    // Case 30: 10 chained symlinks exceed MAXSYMLINKS (8).
    if (expect_err(FAKE_MNT "/l0", LOOKUP_FOLLOW, -ELOOP) != 0)
        return -1;

    // Case 33: the spliced path (254-byte target + "/x" suffix) overflows
    // VFS_NAME_MAX inside splice_symlink_path().
    if (expect_err(FAKE_MNT "/big/x", LOOKUP_FOLLOW, -ENAMETOOLONG) != 0)
        return -1;

    // Case 32: symlink on a filesystem whose ops provide no readlink.
    if (expect_err(FAKE_MNT_NORL "/midlink", LOOKUP_FOLLOW, -EOPNOTSUPP) != 0)
        return -1;

    // Case 37: readdir failure on a verified directory.
    if (expect_err(FAKE_MNT "/baddir/x", LOOKUP_FOLLOW, -EIO) != 0)
        return -1;

    // Sub-mount relative-target resolution.  FAKE_RELLINK's target is the
    // RELATIVE string "dir/target"; without the __vfs_lookup_raw() fix that
    // seeds consumed with mp->path, splice_symlink_path() splices the target
    // against the root mount ("//dir/target") instead of this mount, and
    // resolution fails.  With the fix, FOLLOW resolves to FAKE_TARGET and
    // NOFOLLOW returns the symlink itself.
    if (expect_type(FAKE_MNT "/rellink", LOOKUP_FOLLOW, VFS_FILE) != 0)
        return -1;
    if (expect_type(FAKE_MNT "/rellink", LOOKUP_NOFOLLOW, VFS_SYMLINK) != 0)
        return -1;

    return 0;
}

// ══════════════════════════════════════════════════════════
// Part 2 — ext2 create/unlink rollback and type-aware unlink
// ══════════════════════════════════════════════════════════

#define SLT_FAST_TARGET  "short-target"          // <= 60 bytes → fast symlink
// > 60 bytes → long symlink (one data block allocated for the target).
#define SLT_LONG_TARGET \
    "0123456789012345678901234567890123456789012345678901234567890123456789"

// Returns 0 (and leaves *fs/*root NULL) when the root mount is not ext2 —
// the test then reports PASS as a skip, matching the other ext2 selftests.
static int slt_ext2_root(vfs_node_t **out_root, ext2_fs_t **out_fs)
{
    *out_root = NULL;
    *out_fs = NULL;

    vfs_node_t *root = vfs_lookup("/");
    if (!root) return 0;
    if (root->ops != &ext2_vfs_ops || !root->mount || !root->mount->fs_data) {
        vfs_node_put(root);
        return 0;
    }
    *out_root = root;
    *out_fs = (ext2_fs_t *)root->mount->fs_data;
    return 1;
}

static int slt_name_absent(const char *abs_path)
{
    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, abs_path, LOOKUP_NOFOLLOW, &node);
    if (rc == 0 && node) {
        vfs_node_put(node);
        return 0;   // still there → the failed create left a dirent behind
    }
    return 1;
}

// One fault case: force `fault` to fail, assert -EIO, assert no dirent
// survived, and assert every allocation was rolled back.
static int slt_fault_case(vfs_node_t *root, ext2_fs_t *fs,
                          enum ext2_symlink_test_fault fault,
                          const char *name, const char *abs_path,
                          const char *target)
{
    uint32_t free_inodes = fs->sb_raw.s_free_inodes_count;
    uint32_t free_blocks = fs->sb_raw.s_free_blocks_count;

    ext2_symlink_test_set_fault(fault);
    int rc = root->ops->symlink(root, name, target);
    ext2_symlink_test_set_fault(EXT2_SYMLINK_TEST_NONE);

    if (rc != -EIO) {
        serial_printk("[selftest] symlink_ext2_rollback: fault %d -> %d, expected -EIO\n",
                      (int)fault, rc);
        if (rc == 0) root->ops->unlink(root, name);   // don't leak the dirent
        return -1;
    }
    if (!slt_name_absent(abs_path)) {
        serial_printk("[selftest] symlink_ext2_rollback: fault %d left a dirent\n",
                      (int)fault);
        root->ops->unlink(root, name);
        return -1;
    }
    if (fs->sb_raw.s_free_inodes_count != free_inodes ||
        fs->sb_raw.s_free_blocks_count != free_blocks) {
        serial_printk("[selftest] symlink_ext2_rollback: fault %d leaked "
                      "(inodes %u->%u, blocks %u->%u)\n",
                      (int)fault, free_inodes, fs->sb_raw.s_free_inodes_count,
                      free_blocks, fs->sb_raw.s_free_blocks_count);
        return -1;
    }
    return 0;
}

// One success case: create, check the block delta, unlink, check it again.
// want_block_delta is how many blocks the create must consume: 0 for a fast
// symlink (target inline in i_block[]), 1 for a long symlink.
static int slt_success_case(vfs_node_t *root, ext2_fs_t *fs,
                            const char *name, const char *abs_path,
                            const char *target, uint32_t want_block_delta)
{
    uint32_t free_inodes = fs->sb_raw.s_free_inodes_count;
    uint32_t free_blocks = fs->sb_raw.s_free_blocks_count;

    int rc = root->ops->symlink(root, name, target);
    if (rc != 0) {
        serial_printk("[selftest] symlink_ext2_rollback: symlink(%s) -> %d\n",
                      name, rc);
        return -1;
    }

    uint32_t used_blocks = free_blocks - fs->sb_raw.s_free_blocks_count;
    if (used_blocks != want_block_delta) {
        serial_printk("[selftest] symlink_ext2_rollback: %s used %u blocks, expected %u\n",
                      name, used_blocks, want_block_delta);
        root->ops->unlink(root, name);
        return -1;
    }
    if (slt_name_absent(abs_path)) {
        serial_printk("[selftest] symlink_ext2_rollback: %s not found after create\n",
                      name);
        root->ops->unlink(root, name);
        return -1;
    }

    rc = root->ops->unlink(root, name);
    if (rc != 0) {
        serial_printk("[selftest] symlink_ext2_rollback: unlink(%s) -> %d\n",
                      name, rc);
        return -1;
    }

    // Type-aware unlink: a fast symlink's i_block[] holds target BYTES, so the
    // generic direct-block loop would free random blocks.  The counts must
    // return exactly to where they started — no more, no fewer.
    if (fs->sb_raw.s_free_blocks_count != free_blocks ||
        fs->sb_raw.s_free_inodes_count != free_inodes) {
        serial_printk("[selftest] symlink_ext2_rollback: unlink(%s) mismatched "
                      "(inodes %u->%u, blocks %u->%u)\n",
                      name, free_inodes, fs->sb_raw.s_free_inodes_count,
                      free_blocks, fs->sb_raw.s_free_blocks_count);
        return -1;
    }
    if (!slt_name_absent(abs_path)) {
        serial_printk("[selftest] symlink_ext2_rollback: %s survived unlink\n", name);
        return -1;
    }
    return 0;
}

int symlink_selftest_ext2_rollback(void)
{
    vfs_node_t *root = NULL;
    ext2_fs_t *fs = NULL;
    if (!slt_ext2_root(&root, &fs))
        return 0;   // SKIP — root mount is not ext2 in this image
    if (!root->ops->symlink || !root->ops->unlink) {
        vfs_node_put(root);
        return -1;
    }

    int ret = 0;

    // Every I/O step of ext2_vfs_symlink() that can fail must roll back.
    // WRITE_BLOCK only exists on the long-symlink path, so it gets the long
    // target; the rest are exercised with a fast target.
    static const struct {
        enum ext2_symlink_test_fault fault;
        const char *target;
    } cases[] = {
        { EXT2_SYMLINK_TEST_FIND_DIRENT, SLT_FAST_TARGET },
        { EXT2_SYMLINK_TEST_READ_INODE,  SLT_FAST_TARGET },
        { EXT2_SYMLINK_TEST_WRITE_BLOCK, SLT_LONG_TARGET },
        { EXT2_SYMLINK_TEST_WRITE_INODE, SLT_LONG_TARGET },
        { EXT2_SYMLINK_TEST_DIRENT_ADD,  SLT_FAST_TARGET },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (slt_fault_case(root, fs, cases[i].fault,
                           ".slt_fault", "/.slt_fault", cases[i].target) != 0) {
            ret = -1;
            goto out;
        }
    }

    // Fast symlink: no data block allocated, and unlink must free none.
    if (slt_success_case(root, fs, ".slt_fast", "/.slt_fast",
                         SLT_FAST_TARGET, 0) != 0) {
        ret = -1;
        goto out;
    }

    // Long symlink: exactly one target block allocated, and unlink must
    // release exactly that one.
    if (slt_success_case(root, fs, ".slt_long", "/.slt_long",
                         SLT_LONG_TARGET, 1) != 0) {
        ret = -1;
        goto out;
    }

out:
    ext2_symlink_test_set_fault(EXT2_SYMLINK_TEST_NONE);
    vfs_node_put(root);
    return ret;
}

#else  // !OS01_SELFTEST

int symlink_selftest_resolver(void)      { return 0; }
int symlink_selftest_ext2_rollback(void) { return 0; }

#endif // OS01_SELFTEST
