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
#define USB_MSC_TUR_RETRIES        5

#define SCSI_OP_TEST_UNIT_READY    0x00u
#define SCSI_OP_INQUIRY            0x12u
#define SCSI_OP_READ_CAPACITY_10   0x25u
#define SCSI_OP_READ_10            0x28u

struct usb_msc_dev {
    struct usb_msc_bot bot;
    uint64_t sector_count;
    mutex_t lock;
    block_device_t *bdev;
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
    uint64_t cur_lba = lba;
    uint32_t rem = count;

    while (rem > 0) {
        uint16_t chunk = (rem > USB_MSC_MAX_IO_SECTORS)
                             ? USB_MSC_MAX_IO_SECTORS
                             : (uint16_t)rem;
        size_t want_bytes = (size_t)chunk * USB_MSC_SECTOR_SIZE;
        size_t actual = 0;

        rc = scsi_read_10(m, (uint32_t)cur_lba, chunk, dst, &actual);
        if (rc != 0)
            break;
        if (actual != want_bytes) {
            rc = -EIO;
            break;
        }

        dst += want_bytes;
        cur_lba += chunk;
        rem -= chunk;
    }

    if (locked)
        mutex_unlock(&m->lock);
    return rc;
}

static const struct block_device_ops usb_msc_ops = {
    .read = usb_msc_read,
    .write = NULL,
    .flush = NULL,
};

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

    m->bot.transport = *transport;
    m->bot.interface_num = interface_num;
    m->bot.in_addr = in_addr;
    m->bot.out_addr = out_addr;
    mutex_init(&m->lock);

    int rc = scsi_test_unit_ready(m);
    if (rc != 0) {
        log_warn("usb_msc: %s: TEST UNIT READY failed (rc=%d)\n", name, rc);
        kfree(m);
        return rc;
    }

    char vendor[9], product[17], rev[5];
    rc = scsi_inquiry(m, vendor, product, rev);
    if (rc != 0) {
        log_warn("usb_msc: %s: INQUIRY failed (rc=%d)\n", name, rc);
        kfree(m);
        return rc;
    }

    rc = scsi_read_capacity_10(m, &m->sector_count);
    if (rc != 0) {
        log_warn("usb_msc: %s: READ CAPACITY(10) failed (rc=%d)\n", name, rc);
        kfree(m);
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
        kfree(m);
        return rc;
    }

    m->bdev = bdev;
    if (out)
        *out = bdev;

    log_info("usb_msc: %s: %llu sectors, 512-byte blocks (%s %s %s)\n",
             name, (unsigned long long)m->sector_count, vendor, product, rev);
    return 0;
}
