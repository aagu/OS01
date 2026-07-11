#include <kernel/selftest.h>
#include <kernel/printk.h>
#include <kernel/slab.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <fs/vfs.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <kernel/file.h>
#include <string.h>
#include <stdlib.h>

// ── Linker-defined section boundaries ─────────────────────
// The .selftest_table section is in rodata (linker.ld).
// Symbols are declared extern and placed inside the body —
// they're resolved at link time, not compile time.
extern const selftest_entry_t _selftest_table_start[];
extern const selftest_entry_t _selftest_table_end[];

// ── Built-in tests ────────────────────────────────────────

SELFTEST(test_slab_alloc_free)
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

SELFTEST(test_slab_many_sizes)
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

SELFTEST(test_vfs_mount_root)
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

SELFTEST(test_procfs_read_meminfo)
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

SELFTEST(test_spinlock_basic)
{
    spinlock_T lock;
    spin_init(&lock);
    uint64_t flags;
    flags = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

SELFTEST(test_pipe_basic)
{
    (void)0;
    return 0;
}

// ── Test runner ────────────────────────────────────────────
int selftest_run_all(void)
{
    // Iterate the .selftest_table section populated by SELFTEST() macros.
    // Each entry is compiled into the rodata section; the linker script
    // defines _selftest_table_start / _selftest_table_end boundaries.
    int count  = (int)(_selftest_table_end - _selftest_table_start);
    int passed = 0, failed = 0;

    for (int i = 0; i < count; i++) {
        const char *name = _selftest_table_start[i].name;
        selftest_fn  fn  = _selftest_table_start[i].fn;

        serial_printk("[selftest] %s... ", name);
        int rc = fn();
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
