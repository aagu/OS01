#include <block/blockdev.h>
#include <driver/ahci.h>
#include <kernel/printk.h>
#include <string.h>

// ── Global device table ───────────────────────────────────
static block_device_t block_devices[BLOCKDEV_MAX];
static int block_device_count_val = 0;
static int block_device_initialized = 0;

// ── Default AHCI read/write wrappers ─────────────────────
static int default_ahci_read(block_device_t *dev, uint64_t lba,
                              uint32_t count, void *buf)
{
    return ahci_read_sectors(dev->port_num, lba, count, buf);
}

static int default_ahci_write(block_device_t *dev, uint64_t lba,
                               uint32_t count, const void *buf)
{
    return ahci_write_sectors(dev->port_num, lba, count, buf);
}

// ── Initialization ───────────────────────────────────────
void block_device_init(void)
{
    memset(block_devices, 0, sizeof(block_devices));
    block_device_count_val = 0;
    block_device_initialized = 1;
    serial_printk("block: subsystem initialized\n");
}

// ── Registration ─────────────────────────────────────────
block_device_t *block_device_register(const char *name,
                                       uint32_t port_num,
                                       uint64_t sector_count)
{
    if (!block_device_initialized)
        block_device_init();

    if (block_device_count_val >= BLOCKDEV_MAX) {
        serial_printk("block: max devices reached\n");
        return NULL;
    }

    block_device_t *dev = &block_devices[block_device_count_val++];
    strcpy((char *)dev->name, name);
    dev->port_num = port_num;
    dev->sector_count = sector_count;
    dev->sector_size = 512;
    dev->present = 1;
    dev->read = default_ahci_read;
    dev->write = default_ahci_write;

    serial_printk("block: registered %s (%lu sectors, %lu MB)\n",
                   dev->name, sector_count,
                   sector_count * 512 / 1024 / 1024);
    return dev;
}

// ── Read ─────────────────────────────────────────────────
int block_device_read(block_device_t *dev, uint64_t lba,
                      uint32_t count, void *buffer)
{
    if (!dev || !dev->present || !dev->read) {
        serial_printk("block: read on invalid device\n");
        return -1;
    }
    if (lba + count > dev->sector_count) {
        serial_printk("block: read past end of device\n");
        return -1;
    }
    return dev->read(dev, lba, count, buffer);
}

// ── Write ─────────────────────────────────────────────────
int block_device_write(block_device_t *dev, uint64_t lba,
                       uint32_t count, const void *buffer)
{
    if (!dev || !dev->present || !dev->write) {
        serial_printk("block: write on invalid device\n");
        return -1;
    }
    if (lba + count > dev->sector_count) {
        serial_printk("block: write past end of device\n");
        return -1;
    }
    return dev->write(dev, lba, count, buffer);
}

// ── Enumeration ──────────────────────────────────────────
block_device_t *block_device_get(int index)
{
    if (index < 0 || index >= block_device_count_val)
        return NULL;
    return &block_devices[index];
}

int block_device_count(void)
{
    return block_device_count_val;
}
