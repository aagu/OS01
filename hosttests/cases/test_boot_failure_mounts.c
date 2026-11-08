/* hosttests/cases/test_boot_failure_mounts.c — BOOT-3 Task 2.
 *
 * Verifies root filesystem requirement, optional mount degradation,
 * and error propagation in fs_boot_mounts(), tmpfs_init(), and procfs_init().
 */

#include "test_framework.h"
#include <fs/boot.h>
#include <fs/vfs.h>
#include <fs/gpt.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <block/blockdev.h>
#include <core/boot_failure.h>
#include <errno.h>
#include <string.h>
#include <setjmp.h>

static int stop_calls = 0;
static char last_stop_stage[32];
static char last_stop_component[64];
static int64_t last_stop_rc = 0;
static int last_stop_phase = 0;
static int last_stop_cpu = 0;
static jmp_buf stop_jmp;
static int jump_on_stop = 0;

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    stop_calls++;
    snprintf(last_stop_stage, sizeof(last_stop_stage), "%s", stage ? stage : "unknown");
    snprintf(last_stop_component, sizeof(last_stop_component), "%s", component ? component : "unknown");
    last_stop_rc = rc;
    last_stop_phase = phase;
    last_stop_cpu = cpu;

    if (jump_on_stop) {
        longjmp(stop_jmp, 1);
    }
    abort();
}

static int warn_calls = 0;
static char last_warn_stage[32];
static char last_warn_component[64];
static int64_t last_warn_rc = 0;
static int last_warn_phase = 0;
static int last_warn_cpu = 0;

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    warn_calls++;
    snprintf(last_warn_stage, sizeof(last_warn_stage), "%s", stage ? stage : "unknown");
    snprintf(last_warn_component, sizeof(last_warn_component), "%s", component ? component : "unknown");
    last_warn_rc = rc;
    last_warn_phase = phase;
    last_warn_cpu = cpu;
}

/* Logging mock */
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }
void kpanic(const char *fmt, ...) { (void)fmt; abort(); }

/* Mocks for block device & VFS */
void kfree(void *ptr) { (void)ptr; }
static block_device_t mock_disk;
static int mock_disk_count = 0;
static gpt_info_t *mock_gpt = NULL;
static int mock_ext2_mount_rc = 0;
static int mock_vfat_mount_rc = 0;
static int mock_tmpfs_init_rc = 0;
static int mock_procfs_init_rc = 0;
static int tmpfs_init_calls = 0;
static int procfs_init_calls = 0;

int block_device_count(void) { return mock_disk_count; }
block_device_t *block_device_get(int index)
{
    if (index == 0 && mock_disk_count > 0) return &mock_disk;
    return NULL;
}
int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    (void)name; (void)dev; return 0;
}

gpt_info_t *gpt_scan(block_device_t *disk)
{
    (void)disk;
    return mock_gpt;
}

int vfs_mount_fs(const char *type_name, const char *path,
                 block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    if (strcmp(path, "/") == 0) {
        if (strcmp(type_name, "ext2") == 0) return mock_ext2_mount_rc;
        if (strcmp(type_name, "vfat") == 0) return mock_vfat_mount_rc;
    }
    if (strcmp(path, "/boot") == 0) {
        return mock_vfat_mount_rc;
    }
    return 0;
}

void vfs_init(void) {}
int devfs_init(void) { return 0; }
void vfs_debug_list(const char *path) { (void)path; }
vfs_node_t *vfs_lookup(const char *path) { (void)path; return NULL; }
int vfs_read(vfs_node_t *node, uint64_t off, uint64_t size, void *buf) { (void)node; (void)off; (void)size; (void)buf; return 0; }
int vfs_write(vfs_node_t *node, uint64_t off, uint64_t size, void *buf) { (void)node; (void)off; (void)size; (void)buf; return 0; }
void vfs_node_put(vfs_node_t *node) { (void)node; }

/* In tests where we test fs_boot_mounts, we stub tmpfs_init / procfs_init to track calls */
int tmpfs_init(void)
{
    tmpfs_init_calls++;
    return mock_tmpfs_init_rc;
}

int procfs_init(void)
{
    procfs_init_calls++;
    return mock_procfs_init_rc;
}

static void reset_mount_test_state(void)
{
    stop_calls = 0;
    warn_calls = 0;
    last_stop_stage[0] = '\0';
    last_stop_component[0] = '\0';
    last_stop_rc = 0;
    last_stop_phase = 0;
    last_stop_cpu = 0;
    last_warn_stage[0] = '\0';
    last_warn_component[0] = '\0';
    last_warn_rc = 0;
    last_warn_phase = 0;
    last_warn_cpu = 0;
    jump_on_stop = 0;

    memset(&mock_disk, 0, sizeof(mock_disk));
    mock_disk.kind = BLOCK_DISK;
    strncpy(mock_disk.name, "hda", sizeof(mock_disk.name) - 1);
    mock_disk_count = 0;
    mock_gpt = NULL;
    mock_ext2_mount_rc = 0;
    mock_vfat_mount_rc = 0;
    mock_tmpfs_init_rc = 0;
    mock_procfs_init_rc = 0;
    tmpfs_init_calls = 0;
    procfs_init_calls = 0;
}

static void test_root_missing_disk(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 0;

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(1, stop_calls);
    assert_str_eq("fs", last_stop_stage);
    assert_str_eq("root", last_stop_component);
    assert_eq(-ENODEV, last_stop_rc);
    assert_eq(0, tmpfs_init_calls);
    assert_eq(0, procfs_init_calls);
}

static void test_root_gpt_insufficient_partitions(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 1;

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 1; /* only 1 partition, needs at least 2 */
    mock_gpt = &fake_gpt;

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(1, stop_calls);
    assert_str_eq("fs", last_stop_stage);
    assert_str_eq("root", last_stop_component);
    assert_eq(-EINVAL, last_stop_rc);
    assert_eq(0, tmpfs_init_calls);
    assert_eq(0, procfs_init_calls);
}

static void test_root_ext2_mount_fails(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 1;

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 2;
    mock_gpt = &fake_gpt;
    mock_ext2_mount_rc = -EIO;

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(1, stop_calls);
    assert_str_eq("fs", last_stop_stage);
    assert_str_eq("root", last_stop_component);
    assert_eq(-EIO, last_stop_rc);
    assert_eq(0, tmpfs_init_calls);
    assert_eq(0, procfs_init_calls);
}

static void test_boot_mount_fails_root_continues(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 1;

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 2;
    mock_gpt = &fake_gpt;
    mock_ext2_mount_rc = 0;
    mock_vfat_mount_rc = -EIO; /* /boot fails */

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(0, stop_calls);
    assert_eq(1, warn_calls);
    assert_str_eq("fs", last_warn_stage);
    assert_str_eq("/boot", last_warn_component);
    assert_eq(-EIO, last_warn_rc);
    assert_eq(1, tmpfs_init_calls);
    assert_eq(1, procfs_init_calls);
}

static void test_tmpfs_fails_procfs_still_runs(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 1;

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 2;
    mock_gpt = &fake_gpt;
    mock_ext2_mount_rc = 0;
    mock_vfat_mount_rc = 0;
    mock_tmpfs_init_rc = -ENOMEM; /* tmpfs fails */
    mock_procfs_init_rc = 0;

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(0, stop_calls);
    assert_eq(1, warn_calls);
    assert_str_eq("fs", last_warn_stage);
    assert_str_eq("tmpfs", last_warn_component);
    assert_eq(-ENOMEM, last_warn_rc);
    assert_eq(1, tmpfs_init_calls);
    assert_eq(1, procfs_init_calls);
}

static void test_procfs_fails_warns(void)
{
    reset_mount_test_state();
    jump_on_stop = 1;
    mock_disk_count = 1;

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 2;
    mock_gpt = &fake_gpt;
    mock_ext2_mount_rc = 0;
    mock_vfat_mount_rc = 0;
    mock_tmpfs_init_rc = 0;
    mock_procfs_init_rc = -ENOMEM; /* procfs fails */

    if (setjmp(stop_jmp) == 0) {
        fs_boot_mounts();
    }

    assert_eq(0, stop_calls);
    assert_eq(1, warn_calls);
    assert_str_eq("fs", last_warn_stage);
    assert_str_eq("procfs", last_warn_component);
    assert_eq(-ENOMEM, last_warn_rc);
    assert_eq(1, tmpfs_init_calls);
    assert_eq(1, procfs_init_calls);
}

int main(void)
{
    TEST_SUITE("BOOT-3 boot_failure_mounts");
    test_root_missing_disk();
    test_root_gpt_insufficient_partitions();
    test_root_ext2_mount_fails();
    test_boot_mount_fails_root_continues();
    test_tmpfs_fails_procfs_still_runs();
    test_procfs_fails_warns();
    TEST_RESULTS();
    return 0;
}
