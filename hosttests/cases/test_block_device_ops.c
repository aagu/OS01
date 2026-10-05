/* hosttests/cases/test_block_device_ops.c — ARCH-9 block ops & partition tests */
#include "test_framework.h"
#include <block/blockdev.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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

/* ── Test 1: Complete registration ────────────────────────────── */
TEST_FUNC(test_complete_registration)
{
    block_device_init();
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
    block_device_init();
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

    /* Valid read/write reaches driver */
    assert_eq(0, block_device_read(dev, 10, 5, buffer));
    assert_eq(1, fake_read_calls);
    assert_eq(10, fake_last_lba);
    assert_eq(5, fake_last_count);

    assert_eq(0, block_device_write(dev, 20, 2, buffer));
    assert_eq(1, fake_write_calls);
    assert_eq(20, fake_last_lba);
    assert_eq(2, fake_last_count);
}

/* ── Test 3: Registry failures ────────────────────────────────── */
TEST_FUNC(test_registry_failures)
{
    block_device_init();
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

    /* Fill table up to BLOCKDEV_MAX (8 slots) */
    for (int i = 1; i < BLOCKDEV_MAX; i++) {
        char name[BLOCKDEV_NAME_MAX];
        snprintf(name, sizeof(name), "disk_%d", i);
        struct block_device_desc d = {
            .name = name,
            .sector_count = 100,
            .sector_size = 512,
            .ops = &fake_rw_ops,
            .kind = BLOCK_DISK,
        };
        block_device_t *d_out = NULL;
        assert_eq(0, block_device_register(&d, &d_out));
        assert_not_null(d_out);
    }
    assert_eq(BLOCKDEV_MAX, block_device_count());

    /* Table full -> -ENOSPC */
    struct block_device_desc extra = {
        .name = "disk_overflow",
        .sector_count = 100,
        .sector_size = 512,
        .ops = &fake_rw_ops,
        .kind = BLOCK_DISK,
    };
    out = (block_device_t *)0xdeadbeef;
    assert_eq(-ENOSPC, block_device_register(&extra, &out));
    assert_null(out);
    assert_eq(BLOCKDEV_MAX, block_device_count());
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

static const struct block_device_ops partition_test_ops = {
    .read = partition_fake_read,
    .write = partition_fake_write,
    .flush = NULL,
};

TEST_FUNC(test_partition_parent_dispatch)
{
    block_device_init();
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

    /* Read from partition: lba 10, count 5 -> parent should see LBA 110, count 5 */
    assert_eq(0, block_device_read(part_dev, 10, 5, buffer));
    assert_eq(1, fake_read_calls);
    assert_eq((uint64_t)(100 + 10), fake_last_lba);
    assert_eq(5, fake_last_count);

    /* Write to partition: lba 20, count 2 -> parent should see LBA 120, count 2 */
    assert_eq(0, block_device_write(part_dev, 20, 2, buffer));
    assert_eq(1, fake_write_calls);
    assert_eq((uint64_t)(100 + 20), fake_last_lba);
    assert_eq(2, fake_last_count);

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

/* ── Test 5: Boot unregister stability ────────────────────────── */
TEST_FUNC(test_boot_unregister_stability)
{
    block_device_init();

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

    /* Register a new device ("d3"): reuses the hole left by dev1 without shifting others */
    struct block_device_desc d3 = { .name = "d3", .sector_count = 200, .sector_size = 512, .ops = &fake_rw_ops, .kind = BLOCK_DISK };
    block_device_t *dev3 = NULL;
    assert_eq(0, block_device_register(&d3, &dev3));
    assert_eq(3, block_device_count());
    assert_eq(saved_dev1, dev3); /* Reuses slot 1 */

    /* Check dev0 and dev2 still at same addresses */
    assert_eq(saved_dev0, block_device_get(0));
    assert_eq(saved_dev1, block_device_get(1));
    assert_eq(saved_dev2, block_device_get(2));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_complete_registration),
    TEST_ENTRY(test_readonly_and_bounds),
    TEST_ENTRY(test_registry_failures),
    TEST_ENTRY(test_partition_parent_dispatch),
    TEST_ENTRY(test_boot_unregister_stability),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
