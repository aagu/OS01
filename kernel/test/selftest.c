#include <kernel/selftest.h>
#include <kernel/printk.h>
#include <kernel/slab.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <fs/vfs.h>
#include <kernel/task.h>
#include <kernel/arch/spinlock.h>
#include <kernel/file.h>
#include <string.h>
#include <stdlib.h>

#define SELFTEST_MAX 32
static selftest_entry_t test_table[SELFTEST_MAX];
static int test_count = 0;

void selftest_register(const char *name, selftest_fn fn)
{
    if (test_count >= SELFTEST_MAX) {
        serial_printk("[selftest] ERROR: test table full (%s)\n", name);
        return;
    }
    test_table[test_count].name = name;
    test_table[test_count].fn   = fn;
    test_count++;
}

// ── Built-in tests ────────────────────────────────────────

static int test_slab_alloc_free(void)
{
    void *p1 = kmalloc(64);
    if (!p1) { serial_printk("[selftest] slab_alloc_free: kmalloc returned NULL\n"); return -1; }
    kfree(p1);
    void *p2 = kmalloc(64);
    if (p1 != p2) {
        serial_printk("[selftest] slab_alloc_free: hint: p1=%p != p2=%p\n", p1, p2);
    }
    kfree(p2);
    return 0;
}

static int test_slab_many_sizes(void)
{
    static const int sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    void *ptrs[8];
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            serial_printk("[selftest] slab_many_sizes: kmalloc(%d) returned NULL\n", sizes[i]);
            return -1;
        }
        memset(ptrs[i], 0xAA, sizes[i]);
    }
    for (int i = 0; i < 8; i++)
        kfree(ptrs[i]);
    return 0;
}

static int test_vfs_mount_root(void)
{
    struct vfs_node *root = vfs_lookup("/");
    if (!root) {
        serial_printk("[selftest] vfs_mount_root: vfs_lookup('/') returned NULL\n");
        return -1;
    }
    if (root->type != VFS_DIR) {
        serial_printk("[selftest] vfs_mount_root: '/' is not a directory (type=%d)\n",
                      (int)root->type);
        return -1;
    }
    return 0;
}

static int test_procfs_read_meminfo(void)
{
    struct vfs_node *mi = vfs_lookup("/proc/meminfo");
    if (!mi) {
        serial_printk("[selftest] procfs_read_meminfo: /proc/meminfo not found\n");
        return -1;
    }
    char buf[256];
    int n = vfs_read(mi, 0, sizeof(buf) - 1, buf);
    if (n <= 0) {
        serial_printk("[selftest] procfs_read_meminfo: vfs_read returned %d\n", n);
        return -1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "MemTotal:")) {
        serial_printk("[selftest] procfs_read_meminfo: 'MemTotal:' not found\n");
        return -1;
    }
    return 0;
}

static int test_spinlock_basic(void)
{
    spinlock_T lock;
    spin_init(&lock);
    uint64_t flags;
    flags = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

static int test_pipe_basic(void)
{
    (void)0;
    return 0;
}

// ── External test functions (defined in subsystem .c files) ──
// Forward-declared here instead of polluting public headers.
// Only available when KERNEL_SELFTEST=1 (guarded by OS01_SELFTEST in each .c file).

#ifdef OS01_SELFTEST
int ext2_selftest_magic(void);
int ext2_selftest_struct_sizes(void);
int ext2_selftest_block_alloc(void);
int ext2_selftest_inode_alloc(void);
int ext2_selftest_dirent_roundtrip(void);
int ext2_selftest_write_read(void);
int gpt_selftest_crc32(void);
int tmpfs_selftest_mounted(void);
#endif

// ── Test runner ────────────────────────────────────────────
int selftest_run_all(void)
{
    selftest_register("slab_alloc_free",   test_slab_alloc_free);
    selftest_register("slab_many_sizes",   test_slab_many_sizes);
    selftest_register("vfs_mount_root",    test_vfs_mount_root);
    selftest_register("procfs_read_meminfo", test_procfs_read_meminfo);
    selftest_register("spinlock_basic",    test_spinlock_basic);
    selftest_register("pipe_basic",        test_pipe_basic);

#ifdef OS01_SELFTEST
    selftest_register("ext2_magic",        ext2_selftest_magic);
    selftest_register("ext2_struct_sizes", ext2_selftest_struct_sizes);
    selftest_register("ext2_block_alloc",      ext2_selftest_block_alloc);
    selftest_register("ext2_inode_alloc",      ext2_selftest_inode_alloc);
    selftest_register("ext2_dirent_roundtrip", ext2_selftest_dirent_roundtrip);
    selftest_register("ext2_write_read",       ext2_selftest_write_read);
    selftest_register("gpt_crc32",         gpt_selftest_crc32);
    selftest_register("tmpfs_mounted",     tmpfs_selftest_mounted);
#endif

    int passed = 0, failed = 0;
    for (int i = 0; i < test_count; i++) {
        serial_printk("[selftest] %s... ", test_table[i].name);
        int rc = test_table[i].fn();
        if (rc == 0) {
            serial_printk("PASS\n");
            passed++;
        } else {
            serial_printk("FAIL (%d)\n", rc);
            failed++;
        }
    }
    serial_printk("[selftest] %d total: %d passed, %d failed\n",
                  passed + failed, passed, failed);
    return failed;
}
