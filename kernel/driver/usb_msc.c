/* kernel/driver/usb_msc.c — Generic USB Mass Storage Class driver */
#include <driver/usb_msc.h>
#include <driver/usb_msc_bot.h>
#include <block/blockdev.h>
#include <errno.h>
#include <log/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sync/mutex.h>

#ifndef OS01_HOST_TEST
#include <fs/vfs.h>
#include <memory/slab.h>
#endif

#define USB_MSC_SECTOR_SIZE        512u
#define USB_MSC_MAX_IO_SECTORS     128u
#define USB_MSC_CACHE_SIZE         (USB_MSC_MAX_IO_SECTORS * USB_MSC_SECTOR_SIZE)
#define USB_MSC_TUR_RETRIES        5

#define SCSI_OP_TEST_UNIT_READY    0x00u
#define SCSI_OP_INQUIRY            0x12u
#define SCSI_OP_READ_CAPACITY_10   0x25u
#define SCSI_OP_READ_10            0x28u
#define SCSI_OP_WRITE_10           0x2au
#define SCSI_OP_SYNCHRONIZE_CACHE  0x35u

struct usb_msc_dev {
    struct usb_msc_bot bot;
    uint64_t sector_count;
    mutex_t lock;
    block_device_t *bdev;
    uint8_t *cache_buf;
    uint8_t *scratch_buf;
    uint64_t cache_lba;
    uint32_t cache_count;
    bool cache_valid;
};

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void sanitize_ascii(const uint8_t *src, size_t len, char *dst)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 0x20u && c <= 0x7eu) ? (char)c : ' ';
        if (dst[i] != ' ')
            out = i + 1;
    }
    dst[out] = '\0';
}

static int scsi_test_unit_ready(struct usb_msc_dev *m)
{
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_TEST_UNIT_READY;

    int rc = -EIO;
    for (int i = 0; i < USB_MSC_TUR_RETRIES; i++) {
        size_t actual = 0;
        rc = usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_NONE,
                                 NULL, 0, true, &actual);
        if (rc == 0)
            return 0;
        if (m->bot.dead || rc == -ETIMEDOUT)
            return rc;
    }
    return rc;
}

static int scsi_inquiry(struct usb_msc_dev *m, char *vendor, char *product, char *rev)
{
    uint8_t cdb[6];
    uint8_t inq[36];
    size_t actual = 0;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_INQUIRY;
    cdb[4] = sizeof(inq);

    int rc = usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_IN,
                                 inq, sizeof(inq), true, &actual);
    if (rc != 0)
        return rc;
    if (actual < sizeof(inq))
        return -EIO;

    sanitize_ascii(inq + 8, 8, vendor);
    sanitize_ascii(inq + 16, 16, product);
    sanitize_ascii(inq + 32, 4, rev);
    return 0;
}

static int scsi_read_capacity_10(struct usb_msc_dev *m, uint64_t *out_sectors)
{
    uint8_t cdb[10];
    uint8_t resp[8];
    size_t actual = 0;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_READ_CAPACITY_10;

    int rc = usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_IN,
                                 resp, sizeof(resp), true, &actual);
    if (rc != 0)
        return rc;
    if (actual != sizeof(resp))
        return -EIO;

    uint32_t last_lba = get_be32(resp + 0);
    uint32_t block_size = get_be32(resp + 4);

    if (block_size != USB_MSC_SECTOR_SIZE) {
        log_warn("usb_msc: unsupported logical block size %u\n", block_size);
        return -EINVAL;
    }
    if (last_lba == 0xffffffffu) {
        log_warn("usb_msc: capacity exceeds 32-bit LBA (0xffffffff), unsupported\n");
        return -EINVAL;
    }

    *out_sectors = (uint64_t)last_lba + 1ull;
    return 0;
}

static int scsi_read_10(struct usb_msc_dev *m, uint32_t lba, uint16_t count,
                        void *buf, size_t *actual)
{
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_READ_10;
    put_be32(cdb + 2, lba);
    put_be16(cdb + 7, count);

    return usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_IN,
                               buf, (uint32_t)count * USB_MSC_SECTOR_SIZE,
                               true, actual);
}

static int scsi_write_10(struct usb_msc_dev *m, uint32_t lba, uint16_t count,
                         const void *buf, size_t *actual)
{
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_WRITE_10;
    put_be32(cdb + 2, lba);
    put_be16(cdb + 7, count);

    return usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_OUT,
                               (void *)buf, (uint32_t)count * USB_MSC_SECTOR_SIZE,
                               false, actual);
}

static void msc_invalidate_overlap(struct usb_msc_dev *m, uint64_t lba, uint32_t count)
{
    if (!m->cache_valid)
        return;

    uint64_t cache_end = m->cache_lba + m->cache_count;
    uint64_t write_end = lba + count;
    if (lba < cache_end && write_end > m->cache_lba) {
        m->cache_valid = false;
        m->cache_count = 0;
    }
}

static int msc_refill_window(struct usb_msc_dev *m, uint64_t start_lba, uint32_t min_sectors)
{
    uint64_t avail = m->sector_count - start_lba;
    uint16_t want = (avail > USB_MSC_MAX_IO_SECTORS)
                        ? USB_MSC_MAX_IO_SECTORS
                        : (uint16_t)avail;
    size_t actual = 0;

    int rc = scsi_read_10(m, (uint32_t)start_lba, want, m->scratch_buf, &actual);
    if (rc != 0)
        return rc;

    uint32_t full_sectors = (uint32_t)(actual / USB_MSC_SECTOR_SIZE);
    if (full_sectors > want)
        full_sectors = want;
    if (full_sectors == 0)
        return -EIO;

    memcpy(m->cache_buf, m->scratch_buf, (size_t)full_sectors * USB_MSC_SECTOR_SIZE);
    m->cache_lba = start_lba;
    m->cache_count = full_sectors;
    m->cache_valid = true;

    if (full_sectors < want && full_sectors < min_sectors)
        return -EIO;
    return 0;
}

static int msc_read_direct(struct usb_msc_dev *m, uint64_t lba, uint32_t count, uint8_t *dst)
{
    uint64_t cur_lba = lba;
    uint32_t rem = count;

    while (rem > 0) {
        uint16_t chunk = (rem > USB_MSC_MAX_IO_SECTORS)
                             ? USB_MSC_MAX_IO_SECTORS
                             : (uint16_t)rem;
        size_t want_bytes = (size_t)chunk * USB_MSC_SECTOR_SIZE;
        size_t actual = 0;

        int rc = scsi_read_10(m, (uint32_t)cur_lba, chunk, dst, &actual);
        if (rc != 0)
            return rc;
        if (actual != want_bytes)
            return -EIO;

        dst += want_bytes;
        cur_lba += chunk;
        rem -= chunk;
    }

    return 0;
}

static int usb_msc_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->private_data)
        return -EINVAL;
    if (count == 0)
        return 0;
    if (!buf)
        return -EINVAL;

    struct usb_msc_dev *m = dev->private_data;
    if (lba >= m->sector_count || (uint64_t)count > m->sector_count - lba)
        return -EINVAL;

    bool locked = vfs_sched_ready();
    if (locked)
        mutex_lock(&m->lock);

    int rc = 0;
    uint8_t *dst = buf;

    if (count == 1) {
        if (!m->cache_valid || lba < m->cache_lba || lba >= m->cache_lba + m->cache_count) {
            rc = msc_refill_window(m, lba, 1);
            if (rc != 0)
                goto out;
        }
        memcpy(dst, m->cache_buf + (lba - m->cache_lba) * USB_MSC_SECTOR_SIZE,
               USB_MSC_SECTOR_SIZE);
        goto out;
    }

    if (m->cache_valid && lba >= m->cache_lba && lba < m->cache_lba + m->cache_count) {
        uint64_t cur_lba = lba;
        uint32_t rem = count;
        uint64_t cached_avail = (m->cache_lba + m->cache_count) - cur_lba;
        uint32_t take = (rem < cached_avail) ? rem : (uint32_t)cached_avail;

        memcpy(dst, m->cache_buf + (cur_lba - m->cache_lba) * USB_MSC_SECTOR_SIZE,
               (size_t)take * USB_MSC_SECTOR_SIZE);
        dst += (size_t)take * USB_MSC_SECTOR_SIZE;
        cur_lba += take;
        rem -= take;

        while (rem > 0) {
            rc = msc_refill_window(m, cur_lba, rem);
            if (rc != 0)
                goto out;
            take = (rem < m->cache_count) ? rem : m->cache_count;
            memcpy(dst, m->cache_buf, (size_t)take * USB_MSC_SECTOR_SIZE);
            dst += (size_t)take * USB_MSC_SECTOR_SIZE;
            cur_lba += take;
            rem -= take;
        }
        goto out;
    }

    rc = msc_read_direct(m, lba, count, dst);

out:
    if (locked)
        mutex_unlock(&m->lock);
    return rc;
}

static int usb_msc_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->private_data)
        return -EINVAL;
    if (count == 0)
        return 0;
    if (!buf)
        return -EINVAL;

    struct usb_msc_dev *m = dev->private_data;
    if (lba >= m->sector_count || (uint64_t)count > m->sector_count - lba)
        return -EINVAL;

    bool locked = vfs_sched_ready();
    if (locked)
        mutex_lock(&m->lock);

    int rc = 0;
    const uint8_t *src = buf;
    uint64_t cur_lba = lba;
    uint32_t rem = count;

    while (rem > 0) {
        uint16_t chunk = (rem > USB_MSC_MAX_IO_SECTORS)
                             ? USB_MSC_MAX_IO_SECTORS
                             : (uint16_t)rem;
        size_t want_bytes = (size_t)chunk * USB_MSC_SECTOR_SIZE;
        size_t actual = 0;

        /* Invalidate overlapping read-ahead before sending WRITE(10) so an
         * uncertain/failed write that modifies media cannot leave stale data. */
        msc_invalidate_overlap(m, cur_lba, chunk);

        rc = scsi_write_10(m, (uint32_t)cur_lba, chunk, src, &actual);
        if (rc != 0)
            goto out;
        if (actual != want_bytes) {
            rc = -EIO;
            goto out;
        }

        src += want_bytes;
        cur_lba += chunk;
        rem -= chunk;
    }

out:
    if (locked)
        mutex_unlock(&m->lock);
    return rc;
}

static int usb_msc_flush(block_device_t *dev)
{
    if (!dev || !dev->private_data)
        return -EINVAL;

    struct usb_msc_dev *m = dev->private_data;
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OP_SYNCHRONIZE_CACHE;

    bool locked = vfs_sched_ready();
    if (locked)
        mutex_lock(&m->lock);

    size_t actual = 0;
    int rc = usb_msc_bot_command(&m->bot, cdb, sizeof(cdb), USB_MSC_NONE,
                                 NULL, 0, false, &actual);

    if (locked)
        mutex_unlock(&m->lock);
    return rc;
}

static const struct block_device_ops usb_msc_ops = {
    .read = usb_msc_read,
    .write = usb_msc_write,
    .flush = usb_msc_flush,
};

static void msc_free_dev(struct usb_msc_dev *m)
{
    if (!m)
        return;
    if (m->scratch_buf)
        kfree(m->scratch_buf);
    if (m->cache_buf)
        kfree(m->cache_buf);
    kfree(m);
}

int usb_msc_probe(const struct usb_msc_transport *transport,
                  uint8_t interface_num, uint8_t in_addr, uint8_t out_addr,
                  const char *name, block_device_t **out)
{
    if (out)
        *out = NULL;

    if (!transport || !transport->control_no_data || !transport->bulk || !transport->clear_halt)
        return -EINVAL;
    if (!name || name[0] == '\0')
        return -EINVAL;
    if ((in_addr & 0x80u) == 0 || (out_addr & 0x80u) != 0)
        return -EINVAL;

    struct usb_msc_dev *m = kmalloc(sizeof(*m));
    if (!m)
        return -ENOMEM;
    memset(m, 0, sizeof(*m));

    m->cache_buf = kmalloc(USB_MSC_CACHE_SIZE);
    m->scratch_buf = kmalloc(USB_MSC_CACHE_SIZE);
    if (!m->cache_buf || !m->scratch_buf) {
        msc_free_dev(m);
        return -ENOMEM;
    }

    m->bot.transport = *transport;
    m->bot.interface_num = interface_num;
    m->bot.in_addr = in_addr;
    m->bot.out_addr = out_addr;
    mutex_init(&m->lock);

    int rc = scsi_test_unit_ready(m);
    if (rc != 0) {
        log_warn("usb_msc: %s: TEST UNIT READY failed (rc=%d)\n", name, rc);
        msc_free_dev(m);
        return rc;
    }

    char vendor[9], product[17], rev[5];
    rc = scsi_inquiry(m, vendor, product, rev);
    if (rc != 0) {
        log_warn("usb_msc: %s: INQUIRY failed (rc=%d)\n", name, rc);
        msc_free_dev(m);
        return rc;
    }

    rc = scsi_read_capacity_10(m, &m->sector_count);
    if (rc != 0) {
        log_warn("usb_msc: %s: READ CAPACITY(10) failed (rc=%d)\n", name, rc);
        msc_free_dev(m);
        return rc;
    }

    struct block_device_desc desc = {
        .name = name,
        .sector_count = m->sector_count,
        .sector_size = USB_MSC_SECTOR_SIZE,
        .ops = &usb_msc_ops,
        .private_data = m,
        .parent = NULL,
        .kind = BLOCK_DISK,
    };

    block_device_t *bdev = NULL;
    rc = block_device_register(&desc, &bdev);
    if (rc != 0) {
        log_err("usb_msc: %s: block_device_register failed (rc=%d)\n", name, rc);
        msc_free_dev(m);
        return rc;
    }

    m->bdev = bdev;
    if (out)
        *out = bdev;

    log_info("usb_msc: %s: %lu sectors, 512-byte blocks (%s %s %s)\n",
             name, (unsigned long)m->sector_count, vendor, product, rev);
    return 0;
}
