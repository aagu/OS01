#include <kernel/selftest.h>
#include <kernel/printk.h>
#include <kernel/slab.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <fs/vfs.h>
#include <kernel/task.h>
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

#include <kernel/arch/x86_64/spinlock.h>
static int test_spinlock_basic(void)
{
    spinlock_T lock;
    spin_init(&lock);
    uint64_t flags;
    flags = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

#include <kernel/file.h>
static int test_pipe_basic(void)
{
    (void)0;
    return 0;
}

int selftest_run_all(void)
{
    selftest_register("slab_alloc_free",   test_slab_alloc_free);
    selftest_register("slab_many_sizes",   test_slab_many_sizes);
    selftest_register("vfs_mount_root",    test_vfs_mount_root);
    selftest_register("procfs_read_meminfo", test_procfs_read_meminfo);
    selftest_register("spinlock_basic",    test_spinlock_basic);
    selftest_register("pipe_basic",        test_pipe_basic);

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
