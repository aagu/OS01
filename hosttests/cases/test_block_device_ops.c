/* hosttests/cases/test_block_device_ops.c — ARCH-9 block ops & partition tests
 *
 * ARCH-10: the block registry is a dynamic list of individually
 * allocated objects.  The ARCH-9 ops/bounds coverage is kept as it was
 * and the registry contract (dynamic growth, stable addresses,
 * registration order, duplicate/OOM/EBUSY/EINVAL paths) is covered
 * here. */
#include "test_framework.h"
#include <block/blockdev.h>
#include <block/bcache.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Allocation stub with failure injection ─────────────────────── */
/* The production registry allocates every device object through the
 * slab; the host harness provides the allocator so the OOM path can be
 * exercised. */
static int  s_kmalloc_calls = 0;
static bool s_kmalloc_fail_next = false;

void *kmalloc(size_t size)
{
    s_kmalloc_calls++;
    if (s_kmalloc_fail_next) {
        s_kmalloc_fail_next = false;
        return NULL;
    }
    return calloc(1, size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

static void reset_registry(void)
{
    block_device_reset_for_test();
    s_kmalloc_calls = 0;
    s_kmalloc_fail_next = false;
}

/* ── Fake tracking data ────────────────────────────────────────── */
static int fake_read_calls = 0;
static int fake_write_calls = 0;
static int fake_flush_calls = 0;
static uint64_t fake_last_lba = 0;
static uint32_t fake_last_count = 0;

static int fake_driver_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    fake_read_calls++;
    fake_last_lba = lba;
    fake_last_count = count;
    if (buf && count > 0) {
        memset(buf, 0xAB, count * 512);
    }
    return 0;
}

static int fake_driver_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    (void)buf;
    fake_write_calls++;
    fake_last_lba = lba;
    fake_last_count = count;
    return 0;
}

static int fake_driver_flush(block_device_t *dev)
{
    (void)dev;
    fake_flush_calls++;
    return 0;
}

static const struct block_device_ops fake_rw_ops = {
    .read = fake_driver_read,
    .write = fake_driver_write,
    .flush = fake_driver_flush,
};

static const struct block_device_ops fake_ro_ops = {
    .read = fake_driver_read,
    .write = NULL,
    .flush = NULL,
};

/* Register a disk with a generated name; returns the new object. */
static block_device_t *register_disk(const char *name, uint64_t sectors)
{
    struct block_device_desc desc = {
        .name = name,
        .sector_count = sectors,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *dev = NULL;
    if (block_device_register(&desc, &dev) != 0)
        return NULL;
    return dev;
}

/* ── Test 1: Complete registration ────────────────────────────── */
TEST_FUNC(test_complete_registration)
{
    reset_registry();
    fake_read_calls = 0;
    fake_write_calls = 0;
    fake_flush_calls = 0;

    struct block_device_desc desc = {
        .name = "disk0",
        .sector_count = 2048,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .private_data = (void *)0x1234,
        .parent = NULL,
        .kind = BLOCK_DISK,
    };

    block_device_t *dev = NULL;
    int rc = block_device_register(&desc, &dev);
    assert_eq(0, rc);
    assert_not_null(dev);
    assert_str_eq("disk0", dev->name);
    assert_eq(2048, dev->sector_count);
    assert_eq(512, dev->sector_size);
    assert_eq(1, dev->present);
    assert_eq(BLOCK_DISK, dev->kind);
    assert_true(dev->ops == &fake_rw_ops);
    assert_true(dev->private_data == (void *)0x1234);
    assert_null(dev->parent);
    assert_eq(0, dev->last_error);

    assert_eq(1, block_device_count());
    assert_eq(dev, block_device_get(0));

    assert_eq(0, block_device_flush(dev));
    assert_eq(1, fake_flush_calls);

    block_device_mark_failed(dev, -EIO);
    assert_eq(-EIO, dev->last_error);
}

/* ── Test 2: Read-only and bounds ─────────────────────────────── */
TEST_FUNC(test_readonly_and_bounds)
{
    reset_registry();
    fake_read_calls = 0;
    fake_write_calls = 0;

    struct block_device_desc rw_desc = {
        .name = "hda",
        .sector_count = 100,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .private_data = NULL,
        .parent = NULL,
        .kind = BLOCK_DISK,
    };
    block_device_t *dev = NULL;
    assert_eq(0, block_device_register(&rw_desc, &dev));
    assert_not_null(dev);

    struct block_device_desc ro_desc = {
        .name = "ro_disk",
        .sector_count = 100,
        .sector_size = 512,
        .ops = &fake_ro_ops,
        .private_data = NULL,
        .parent = NULL,
        .kind = BLOCK_DISK,
    };
    block_device_t *ro_dev = NULL;
    assert_eq(0, block_device_register(&ro_desc, &ro_dev));
    assert_not_null(ro_dev);

    uint8_t buffer[4096] = {0};


    /* Key assertion 1: write to ro_dev returns -EROFS */
    assert_eq(-EROFS, block_device_write(ro_dev, 0, 1, buffer));
    assert_eq(0, fake_write_calls);

    /* Key assertion 2: legal boundary count=0 -> 0 without calling driver */
    assert_eq(0, block_device_read(dev, dev->sector_count, 0, NULL));
    assert_eq(0, fake_read_calls);
    assert_eq(0, block_device_write(dev, dev->sector_count, 0, NULL));
    assert_eq(0, fake_write_calls);

    /* count=0 at lba 0 is also valid */
    assert_eq(0, block_device_read(dev, 0, 0, NULL));
    assert_eq(0, fake_read_calls);

    /* Non-zero NULL buffer -> -EINVAL */
    assert_eq(-EINVAL, block_device_read(dev, 0, 1, NULL));
    assert_eq(-EINVAL, block_device_write(dev, 0, 1, NULL));
    assert_eq(0, fake_read_calls);
    assert_eq(0, fake_write_calls);

    /* lba = UINT64_MAX non-zero request -> -EINVAL */
    assert_eq(-EINVAL, block_device_read(dev, UINT64_MAX, 1, buffer));
    assert_eq(-EINVAL, block_device_write(dev, UINT64_MAX, 1, buffer));
    assert_eq(0, fake_read_calls);
    assert_eq(0, fake_write_calls);

    /* Out of bounds requests */
    assert_eq(-EINVAL, block_device_read(dev, dev->sector_count, 1, buffer));
    assert_eq(-EINVAL, block_device_write(dev, dev->sector_count, 1, buffer));
    assert_eq(-EINVAL, block_device_read(dev, 50, 51, buffer));
    assert_eq(-EINVAL, block_device_write(dev, 50, 51, buffer));
    assert_eq(0, fake_read_calls);
    assert_eq(0, fake_write_calls);

    /* Valid read reaches driver per-sector (ARCH-6 cache split) */
    assert_eq(0, block_device_read(dev, 10, 5, buffer));
    assert_eq(5, fake_read_calls);
    assert_eq(14, fake_last_lba);
    assert_eq(1, fake_last_count);
    /* Second identical read is served from the cache: no backend calls. */
    assert_eq(0, block_device_read(dev, 10, 5, buffer));
    assert_eq(5, fake_read_calls);

    /* Write is write-back: nothing reaches the driver until a flush. */
    assert_eq(0, block_device_write(dev, 20, 2, buffer));
    assert_eq(0, fake_write_calls);
    assert_eq(0, block_device_flush(dev));
    assert_eq(2, fake_write_calls);
    /* Flush drains in slot order — either dirty sector may be written last. */
    assert_true(fake_last_lba == 20 || fake_last_lba == 21);
    assert_eq(1, fake_last_count);
}

/* ── Test 3: Registry failures ────────────────────────────────── */
TEST_FUNC(test_registry_failures)
{
    reset_registry();
    block_device_t *out = (block_device_t *)0xdeadbeef;

    /* NULL descriptor */
    assert_eq(-EINVAL, block_device_register(NULL, &out));
    assert_null(out);
    assert_eq(0, block_device_count());

    /* Empty name */
    struct block_device_desc bad_desc = {
        .name = "",
        .sector_count = 10,
        .sector_size = 512,
        .ops = &fake_rw_ops,
    };
    out = (block_device_t *)0xdeadbeef;
    assert_eq(-EINVAL, block_device_register(&bad_desc, &out));
    assert_null(out);
    assert_eq(0, block_device_count());

    /* Name too long (>= BLOCKDEV_NAME_MAX, which is 16) */
    bad_desc.name = "very_long_name_exceeding_16";
    out = (block_device_t *)0xdeadbeef;
    assert_eq(-EINVAL, block_device_register(&bad_desc, &out));
    assert_null(out);
    assert_eq(0, block_device_count());

    /* Missing read op (read is required) */
    struct block_device_ops bad_ops = {
        .read = NULL,
        .write = fake_driver_write,
        .flush = NULL,
    };
    bad_desc.name = "disk_noread";
    bad_desc.ops = &bad_ops;
    out = (block_device_t *)0xdeadbeef;
    assert_eq(-EINVAL, block_device_register(&bad_desc, &out));
    assert_null(out);
    assert_eq(0, block_device_count());

    /* Register a valid device */
    struct block_device_desc valid_desc = {
        .name = "disk_a",
        .sector_count = 10,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&valid_desc, &out));
    assert_not_null(out);
    assert_eq(1, block_device_count());

    /* Duplicate name -> -EEXIST */
    out = (block_device_t *)0xdeadbeef;
    assert_eq(-EEXIST, block_device_register(&valid_desc, &out));
    assert_null(out);
    assert_eq(1, block_device_count());
}

/* ── Test 4: Partition parent dispatch ────────────────────────── */
typedef struct test_partition_ctx {
    block_device_t *parent;
    uint64_t offset_lba;
    uint64_t length;
} test_partition_ctx_t;

static int partition_fake_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    test_partition_ctx_t *ctx = (test_partition_ctx_t *)dev->private_data;
    return block_device_read(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_fake_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    test_partition_ctx_t *ctx = (test_partition_ctx_t *)dev->private_data;
    return block_device_write(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_fake_flush(block_device_t *dev)
{
    test_partition_ctx_t *ctx = (test_partition_ctx_t *)dev->private_data;
    return block_device_flush(ctx->parent);
}

static const struct block_device_ops partition_test_ops = {
    .read = partition_fake_read,
    .write = partition_fake_write,
    .flush = partition_fake_flush,
};

TEST_FUNC(test_partition_parent_dispatch)
{
    reset_registry();
    fake_read_calls = 0;
    fake_write_calls = 0;
    fake_last_lba = 0;
    fake_last_count = 0;

    /* Register parent disk: 1000 sectors */
    struct block_device_desc parent_desc = {
        .name = "hda",
        .sector_count = 1000,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *parent_dev = NULL;
    assert_eq(0, block_device_register(&parent_desc, &parent_dev));
    assert_not_null(parent_dev);

    /* Register partition: starts at LBA 100, length 50 */
    test_partition_ctx_t part_ctx = {
        .parent = parent_dev,
        .offset_lba = 100,
        .length = 50,
    };
    struct block_device_desc part_desc = {
        .name = "hda1",
        .sector_count = 50,
        .sector_size = 512,
        .ops = &partition_test_ops,
        .private_data = &part_ctx,
        .parent = parent_dev,
        .kind = BLOCK_PARTITION,
    };
    block_device_t *part_dev = NULL;
    assert_eq(0, block_device_register(&part_desc, &part_dev));
    assert_not_null(part_dev);
    assert_eq(BLOCK_PARTITION, part_dev->kind);
    assert_eq(parent_dev, part_dev->parent);

    uint8_t buffer[4096] = {0};

    /* Read from partition: lba 10, count 5 -> per-sector calls at the
       parent, LBAs 110..114, count 1 each (cached under the disk key). */
    assert_eq(0, block_device_read(part_dev, 10, 5, buffer));
    assert_eq(5, fake_read_calls);
    assert_eq((uint64_t)(100 + 14), fake_last_lba);
    assert_eq(1, fake_last_count);

    /* Write to partition: lba 20, count 2 -> write-back caches at the
       parent disk key; a flush drains per-sector (LBA 120 and 121). */
    assert_eq(0, block_device_write(part_dev, 20, 2, buffer));
    assert_eq(0, fake_write_calls);
    assert_eq(0, block_device_flush(part_dev));
    assert_eq(2, fake_write_calls);
    /* Slot-order drain: either dirty sector may land last. */
    assert_true(fake_last_lba == 120 || fake_last_lba == 121);
    assert_eq(1, fake_last_count);

    /* Out of bounds read on partition (length is 50, reading 45 + 10 = 55):
       Block layer catches it before calling partition_read, parent ops NOT called */
    fake_read_calls = 0;
    fake_write_calls = 0;
    assert_eq(-EINVAL, block_device_read(part_dev, 45, 10, buffer));
    assert_eq(0, fake_read_calls);

    /* Out of bounds write on partition (lba 50, count 1 past end) */
    assert_eq(-EINVAL, block_device_write(part_dev, 50, 1, buffer));
    assert_eq(0, fake_write_calls);
}

/* ── Test 5: Dynamic growth keeps published objects stable ─────── */
TEST_FUNC(test_dynamic_growth_stability)
{
    reset_registry();
    fake_read_calls = 0;
    fake_write_calls = 0;

    block_device_t *first = NULL;

    /* Register more devices than the old fixed table could hold. */
    for (int i = 0; i < 12; i++) {
        char name[BLOCKDEV_NAME_MAX];
        snprintf(name, sizeof(name), "grow%d", i);
        block_device_t *dev = register_disk(name, 64);
        assert_not_null(dev);
        if (i == 0)
            first = dev;
    }

    assert_eq(12, block_device_count());

    /* The first object did not move and is still enumerated first. */
    assert_not_null(first);
    assert_eq(first, block_device_get(0));
    assert_str_eq("grow0", first->name);
    assert_eq(1, first->present);

    /* Its data path is unchanged after the registry grew. */
    uint8_t buffer[512];
    memset(buffer, 0, sizeof(buffer));
    assert_eq(0, block_device_read(first, 3, 1, buffer));
    assert_eq(1, fake_read_calls);
    assert_eq(3, fake_last_lba);
    assert_eq(1, fake_last_count);
    assert_eq(0xAB, buffer[0]);

    assert_eq(0, block_device_write(first, 7, 1, buffer));
    assert_eq(0, fake_write_calls);            // write-back: resident dirty
    assert_eq(0, block_device_flush(first));
    assert_eq(1, fake_write_calls);
    assert_eq(7, fake_last_lba);
    assert_eq(1, fake_last_count);

    /* The enumeration follows the successful registration order. */
    for (int i = 0; i < 12; i++) {
        char name[BLOCKDEV_NAME_MAX];
        snprintf(name, sizeof(name), "grow%d", i);
        block_device_t *dev = block_device_get(i);
        assert_not_null(dev);
        if (!dev)
            continue;
        assert_str_eq(name, dev->name);
    }
    assert_null(block_device_get(12));
    assert_null(block_device_get(-1));
}

/* ── Test 6: Duplicate name is rejected ───────────────────────── */
TEST_FUNC(test_duplicate_name_rejected)
{
    reset_registry();

    block_device_t *first = register_disk("dup", 10);
    assert_not_null(first);
    assert_eq(1, block_device_count());

    struct block_device_desc dup_desc = {
        .name = "dup",
        .sector_count = 10,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *out = (block_device_t *)0xdeadbeef;
    assert_eq(-EEXIST, block_device_register(&dup_desc, &out));
    assert_null(out);

    /* The registered set is unchanged. */
    assert_eq(1, block_device_count());
    assert_eq(first, block_device_get(0));
}

/* ── Test 7: Allocation failure reports -ENOMEM ───────────────── */
TEST_FUNC(test_allocation_failure)
{
    reset_registry();

    block_device_t *first = register_disk("keep", 10);
    assert_not_null(first);
    assert_eq(1, block_device_count());

    struct block_device_desc desc = {
        .name = "oom",
        .sector_count = 10,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };

    s_kmalloc_fail_next = true;
    block_device_t *out = (block_device_t *)0xdeadbeef;
    assert_eq(-ENOMEM, block_device_register(&desc, &out));
    assert_null(out);

    /* Exactly one allocation attempt was made for the failed device. */
    assert_eq(2, s_kmalloc_calls);

    /* The failed registration changed neither the count nor the set. */
    assert_eq(1, block_device_count());
    assert_eq(first, block_device_get(0));
    assert_null(block_device_get(1));
}

/* ── Test 8: Parent with live children is busy ────────────────── */
TEST_FUNC(test_parent_child_rollback)
{
    reset_registry();

    block_device_t *parent = register_disk("disk", 1000);
    assert_not_null(parent);

    test_partition_ctx_t part_ctx = {
        .parent = parent,
        .offset_lba = 100,
        .length = 50,
    };
    struct block_device_desc part_desc = {
        .name = "disk1",
        .sector_count = 50,
        .sector_size = 512,
        .ops = &partition_test_ops,
        .private_data = &part_ctx,
        .parent = parent,
        .kind = BLOCK_PARTITION,
    };
    block_device_t *child = NULL;
    assert_eq(0, block_device_register(&part_desc, &child));
    assert_not_null(child);
    assert_eq(2, block_device_count());

    /* The parent still has a registered partition child -> -EBUSY. */
    assert_eq(-EBUSY, block_device_unregister_boot(parent));
    assert_eq(2, block_device_count());
    assert_eq(parent, block_device_get(0));
    assert_eq(child, block_device_get(1));

    /* Roll back child first, then the parent. */
    assert_eq(0, block_device_unregister_boot(child));
    assert_eq(1, block_device_count());
    assert_eq(parent, block_device_get(0));

    assert_eq(0, block_device_unregister_boot(parent));
    assert_eq(0, block_device_count());
    assert_null(block_device_get(0));
}

/* ── Test 9: A foreign pointer is rejected without dereference ── */
TEST_FUNC(test_unregister_non_member)
{
    reset_registry();

    block_device_t *dev = register_disk("member", 10);
    assert_not_null(dev);
    assert_eq(1, block_device_count());

    /* A stack object that is not a registry member: the call must not
     * read or write it. */
    struct block_device foreign;
    memset(&foreign, 0x5A, sizeof(foreign));
    struct block_device snapshot = foreign;
    assert_eq(-EINVAL, block_device_unregister_boot(&foreign));
    assert_eq(0, memcmp(&foreign, &snapshot, sizeof(foreign)));

    /* A wild pointer: dereferencing it would fault the test. */
    block_device_t *wild = (block_device_t *)0xdeadbeef;
    assert_eq(-EINVAL, block_device_unregister_boot(wild));

    /* NULL is rejected as well. */
    assert_eq(-EINVAL, block_device_unregister_boot(NULL));

    /* The registry is untouched. */
    assert_eq(1, block_device_count());
    assert_eq(dev, block_device_get(0));
}

/* ── Test 10: Boot unregister stability and ordering ──────────── */
TEST_FUNC(test_boot_unregister_stability)
{
    reset_registry();

    struct block_device_desc d0 = { .name = "d0", .sector_count = 100, .sector_size = 512, .ops = &fake_rw_ops, .kind = BLOCK_DISK };
    struct block_device_desc d1 = { .name = "d1", .sector_count = 100, .sector_size = 512, .ops = &fake_rw_ops, .kind = BLOCK_DISK };
    struct block_device_desc d2 = { .name = "d2", .sector_count = 100, .sector_size = 512, .ops = &fake_rw_ops, .kind = BLOCK_DISK };

    block_device_t *dev0 = NULL;
    block_device_t *dev1 = NULL;
    block_device_t *dev2 = NULL;

    assert_eq(0, block_device_register(&d0, &dev0));
    assert_eq(0, block_device_register(&d1, &dev1));
    assert_eq(0, block_device_register(&d2, &dev2));
    assert_eq(3, block_device_count());

    /* Save the addresses of dev0, dev1, dev2 */
    block_device_t *saved_dev0 = dev0;
    block_device_t *saved_dev1 = dev1;
    block_device_t *saved_dev2 = dev2;

    /* Unregister dev1 */
    assert_eq(0, block_device_unregister_boot(dev1));
    assert_eq(2, block_device_count());

    /* Assert dev0 and dev2 pointers did NOT move */
    assert_eq(saved_dev0, block_device_get(0));
    assert_eq(saved_dev2, block_device_get(1));
    assert_true(saved_dev1 != block_device_get(0));
    assert_true(saved_dev1 != block_device_get(1));
    assert_eq(0, saved_dev1->present);

    /* Calling read/write on unregistered dev1 returns -EINVAL */
    uint8_t buf[512];
    assert_eq(-EINVAL, block_device_read(saved_dev1, 0, 1, buf));
    assert_eq(-EINVAL, block_device_write(saved_dev1, 0, 1, buf));

    /* Register a new device ("d3"): it is appended at the end of the
       enumeration order and does not take the hole dev1 left behind. */
    struct block_device_desc d3 = { .name = "d3", .sector_count = 200, .sector_size = 512, .ops = &fake_rw_ops, .kind = BLOCK_DISK };
    block_device_t *dev3 = NULL;
    assert_eq(0, block_device_register(&d3, &dev3));
    assert_eq(3, block_device_count());

    /* Check dev0 and dev2 still at same addresses, d3 is last */
    assert_eq(saved_dev0, block_device_get(0));
    assert_eq(saved_dev2, block_device_get(1));
    assert_eq(dev3, block_device_get(2));
    assert_str_eq("d3", block_device_get(2)->name);
}

/* ── Test 11: Initialization is idempotent ────────────────────── */
TEST_FUNC(test_init_is_idempotent)
{
    reset_registry();

    block_device_t *dev = register_disk("init_disk", 10);
    assert_not_null(dev);
    assert_eq(1, block_device_count());

    /* Re-initializing must not reset the registry or drop the object. */
    block_device_init();
    block_device_init();
    assert_eq(1, block_device_count());
    assert_eq(dev, block_device_get(0));
    assert_eq(1, dev->present);
    assert_str_eq("init_disk", dev->name);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_complete_registration),
    TEST_ENTRY(test_readonly_and_bounds),
    TEST_ENTRY(test_registry_failures),
    TEST_ENTRY(test_partition_parent_dispatch),
    TEST_ENTRY(test_dynamic_growth_stability),
    TEST_ENTRY(test_duplicate_name_rejected),
    TEST_ENTRY(test_allocation_failure),
    TEST_ENTRY(test_parent_child_rollback),
    TEST_ENTRY(test_unregister_non_member),
    TEST_ENTRY(test_boot_unregister_stability),
    TEST_ENTRY(test_init_is_idempotent),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
