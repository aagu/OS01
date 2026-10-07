#include <core/selftest.h>
#include <selftest/result.h>   /* selftest_register / _register_late / coordinator */
#include <core/printk.h>
#include <memory/slab.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <fs/vfs.h>
#include <sched/task.h>
#include <arch/spinlock.h>
#include <fs/file.h>
#include <string.h>
#include <stdlib.h>

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

#if !defined(__aarch64__)
/* x86_64-only bodies: vfs_lookup/vfs_read live in fs/*.c, which is not
 * in the aarch64 kernel source whitelist. */
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
#endif /* !__aarch64__ */

static int test_spinlock_basic(void)
{
    spinlock_T lock;
    spin_init(&lock);
    uint64_t flags;
    flags = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

/* NOTE: the former no-op pipe test (which returned 0 and so produced a
 * bogus PASS) is deliberately gone from both the body list and the
 * registration below.  Spec §5.2: an unimplemented test must be removed
 * from registration or reported as a SKIP with a reason — it must never
 * emit PASS.  Re-add a real pipe test here, registered below, when one
 * exists. */

int test_rwlock_basic(void);
int test_seqlock_basic(void);
int test_slab_16_caches(void);
int test_aarch64_page_table_selftest(void);

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
int ext2_selftest_sparse_read(void);
int gpt_selftest_crc32(void);
int tmpfs_selftest_mounted(void);
int test_timer_tsc_freq(void);
int test_timer_jiffies_hz(void);
int selftest_uaccess(void);
int symlink_selftest_resolver(void);
int symlink_selftest_ext2_rollback(void);
int deep_copy_argv_selftest_empty(void);
int deep_copy_argv_selftest_overcap(void);
int at_random_selftest_layout(void);
int at_random_selftest_layout_even(void);
int at_random_selftest_entropy(void);
int test_at_random_strong_only(void);
int test_canary_single_source(void);
int test_arch_atomic_u64_or_and(void);
int entropy_quality_selftest_current_mode(void);
#endif

// ── Registration ───────────────────────────────────────────
// Populated once per run by selftest_begin_run() (kernel/selftest/result.c),
// which then executes the early subset via selftest_run_all().  The
// protocol publisher emits START/SELECT/BEGIN/terminal/END around this
// selection; the `[selftest] ...` lines are diagnostics only.
void selftest_register_builtin_tests(void)
{
    selftest_register("slab_alloc_free",   test_slab_alloc_free);
    selftest_register("slab_many_sizes",   test_slab_many_sizes);
    /* M2 Task 6: all 16 kmalloc caches. Portable — kmalloc/kfree and
     * the cache table exist on both aarch64 and x86_64. Prints the
     * parser-asserted '[selftest] slab: 16/16 PASS' marker. */
    selftest_register("slab_16_caches",    test_slab_16_caches);
#if defined(__aarch64__)
    /* M3.4 Task 22: aarch64 VMM change primitive coverage on a
     * kernel-internal SCRATCH root. Exercises gic_target_bit
     * cache, map/update/unmap/query 4K, map/unmap 2M block, and
     * split_block_2m (the unpublished-root path). Prints the
     * parser-asserted '[selftest] m3: 4/4 PASS' marker; on
     * x86_64 the body is a no-op stub so registration stays
     * portable. */
    selftest_register("aarch64_pt_vmm",            test_aarch64_page_table_selftest);
#endif /* __aarch64__ */
#if !defined(__aarch64__)
    /* x86_64-only: these tests pull in subsystems (VFS, ext2, sync
     * primitives, TSC timer, ...) that are not in the aarch64 kernel
     * source whitelist, so registering them there would break the
     * link. */
    selftest_register("vfs_mount_root",    test_vfs_mount_root);
    selftest_register("procfs_read_meminfo", test_procfs_read_meminfo);
#endif /* !__aarch64__ */
    selftest_register("spinlock_basic",    test_spinlock_basic);
#if !defined(__aarch64__)
    /* Bodies in selftest/test_sync.c (not in the aarch64 whitelist). */
    selftest_register("rwlock_basic",      test_rwlock_basic);
    selftest_register("seqlock_basic",     test_seqlock_basic);
#endif /* !__aarch64__ */

#ifdef OS01_SELFTEST
#if !defined(__aarch64__)
    selftest_register("ext2_magic",        ext2_selftest_magic);
    selftest_register("ext2_struct_sizes", ext2_selftest_struct_sizes);
    selftest_register("ext2_block_alloc",      ext2_selftest_block_alloc);
    selftest_register("ext2_inode_alloc",      ext2_selftest_inode_alloc);
    selftest_register("ext2_dirent_roundtrip", ext2_selftest_dirent_roundtrip);
    selftest_register("ext2_write_read",       ext2_selftest_write_read);
    selftest_register("ext2_sparse_read",      ext2_selftest_sparse_read);
    selftest_register("gpt_crc32",         gpt_selftest_crc32);
    selftest_register("tmpfs_mounted",     tmpfs_selftest_mounted);
    selftest_register("timer_tsc_freq",    test_timer_tsc_freq);
    selftest_register("timer_jiffies_hz",  test_timer_jiffies_hz);
    selftest_register("uaccess",           selftest_uaccess);
    selftest_register("symlink_resolver",  symlink_selftest_resolver);
    selftest_register("symlink_ext2_rollback", symlink_selftest_ext2_rollback);
    selftest_register("deep_copy_argv_empty", deep_copy_argv_selftest_empty);
    selftest_register("deep_copy_argv_overcap", deep_copy_argv_selftest_overcap);
    selftest_register("at_random_layout",       at_random_selftest_layout);
    selftest_register("at_random_layout_even",  at_random_selftest_layout_even);
    selftest_register("at_random_entropy",      at_random_selftest_entropy);
    selftest_register("at_random_strong_only",  test_at_random_strong_only);
    selftest_register("canary_single_source",   test_canary_single_source);
    selftest_register("arch_atomic_u64_or_and", test_arch_atomic_u64_or_and);
    selftest_register("entropy_quality_selftest_current_mode",
                      entropy_quality_selftest_current_mode);
#endif /* !__aarch64__ */

    /* ── Scheduled (late) cases ─────────────────────────────────
     * Executed from task_init() in kernel/sched/core.c once the
     * scheduler is live and reported with selftest_record_late().
     * These ids are the two-sided contract with the record calls in
     * sched/core.c: a declared late case that never runs is a failure,
     * so a drifting id cannot silently drop a scheduled test.  AArch64
     * has no scheduler-side tests, so it declares none. */
#if !defined(__aarch64__)
    selftest_register_late("kernel_mutex");
    selftest_register_late("kthread_self_reap");
    selftest_register_late("fd_refcount");
    selftest_register_late("pgrp_signal");
    selftest_register_late("tty_vintr");
#endif /* !__aarch64__ */
#endif /* OS01_SELFTEST */
}
