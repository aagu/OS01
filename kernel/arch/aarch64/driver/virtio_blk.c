// kernel/arch/aarch64/driver/virtio_blk.c — virtio-blk over virtio-mmio.
//
// The aarch64 BLOCK_DISK transport: QEMU virt exposes virtio,mmio slots at
// 0x0a000000 + i*0x200 (SPI 48 + i), just above the early Device window
// [0x08000000, 0x0a000000).  The runtime direct map stops at 0x0a000000
// (runtime_tree.c D window is half-open), so this driver maps the first
// 2 MiB of the virtio window into TTBR1 as Device memory — the same
// hand-rolled approach the framebuffer backend uses (printk_fb.c) — and
// keeps the PA↔VA identity of the direct-map convention so Phy_To_Virt()
// reaches the registers.
//
// Device discovery comes from the DTB (dtb_virtio_mmio_* accessors): each
// slot is probed for MAGIC 'virt' / VersionID 2 / DeviceID 2 (block).
// Queue setup and transfers follow virtio 1.0 split-virtqueue semantics
// with modern (non-legacy) virtio-mmio register layout; completion is
// polled on the used ring, so no GIC IRQ is armed yet.
//
// Logging policy: operational paths use log_info/log_warn/log_err
// (docs/log/log.md).

#include <arch/aarch64/virtio_blk.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/boot_direct_map.h>  // aarch64_read_ttbr1
#include <arch/mmu.h>
#include <arch/mmu.h>
#include <arch/spinlock.h>
#include <block/blockdev.h>
#include <memory/pmm.h>
#include <memory/memory.h>     // Phy_To_Virt, PAGE_* constants
#include <subsys/subsys.h>
#include <log/log.h>
#include <stdlib.h>          // calloc/free
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ── virtio-mmio v2 register map (Linux drivers/virtio/virtio_mmio.h layout)
#define VMMIO_MAGIC              0x000
#define VMMIO_VERSION            0x004
#define VMMIO_DEVICE_ID          0x008
#define VMMIO_DEVICE_FEATURES    0x010
#define VMMIO_DEVICE_FEATURES_SEL 0x014
#define VMMIO_DRIVER_FEATURES    0x020
#define VMMIO_DRIVER_FEATURES_SEL 0x024
#define VMMIO_QUEUE_SEL          0x030
#define VMMIO_QUEUE_NUM_MAX      0x034
#define VMMIO_QUEUE_NUM          0x038
#define VMMIO_QUEUE_READY        0x044
#define VMMIO_QUEUE_NOTIFY       0x050
#define VMMIO_STATUS             0x070
#define VMMIO_QUEUE_DESC_LOW     0x080
#define VMMIO_QUEUE_DESC_HIGH    0x084
#define VMMIO_QUEUE_AVAIL_LOW    0x090
#define VMMIO_QUEUE_AVAIL_HIGH   0x094
#define VMMIO_QUEUE_USED_LOW     0x0a0
#define VMMIO_QUEUE_USED_HIGH    0x0a4
#define VMMIO_CONFIG             0x100   // blk config: capacity (u64 LE)

// ── virtio status / feature / request constants
#define VIRTIO_S_ACKNOWLEDGE     1u
#define VIRTIO_S_DRIVER          2u
#define VIRTIO_S_DRIVER_OK       4u
#define VIRTIO_S_FEATURES_OK     8u

#define VIRTIO_F_VERSION_1       32u     // feature bit (word 1, bit 0)
#define VIRTIO_BLK_T_IN          0u      // device writes the data buffer
#define VIRTIO_BLK_T_OUT         1u      // device reads the data buffer
#define VIRTIO_BLK_S_OK          0u

#define VRING_DESC_F_NEXT        1u
#define VRING_DESC_F_WRITE       2u

#define VIRTIO_BLK_QUEUE_SIZE    16u     // in-flight descriptor chains
#define VIRTIO_BLK_SECTOR        512u
// Poll budget: a QEMU virtio-blk request completes in microseconds; this
// budget only bounds a wedged device, so it is generous.
#define VIRTIO_BLK_POLL_LIMIT    20000000u

// Maximum transfer this driver accepts per request (sector-terminated 4 KiB
// page copies from FAT/ext2 stay far below this).
#define VIRTIO_BLK_MAX_SECTORS   64u

struct virtio_blk_dev {
    volatile uint32_t *regs;         // Phy_To_Virt(slot->base)
    uint64_t           base;         // slot PA (diagnostics)
    // Split-virtqueue ring buffers (all 4 KiB PMM pages, PA stored for the
    // device, VA for the kernel).
    uint64_t           desc_pa, avail_pa, used_pa;
    volatile void     *desc, *avail, *used;
    uint32_t           queue_size;
    // Free-list of descriptor chains: chain i owns descriptors
    // [i*3, i*3+2] and its private scratch buffers.
    uint8_t           *hdr;          // [chains][16]
    uint8_t           *status;       // [chains][1]  (device-writable byte)
    uint64_t           hdr_pa, status_pa;
    spinlock_T         lock;
    uint16_t           last_used_idx;
    bool               ready;
};

#define VIRTIO_BLK_MAX_DEVS 4
static struct virtio_blk_dev *s_devs[VIRTIO_BLK_MAX_DEVS];
static uint32_t s_dev_count;

static inline uint32_t r32(struct virtio_blk_dev *d, uint32_t off)
{
    return d->regs[off / 4];
}
static inline void w32(struct virtio_blk_dev *d, uint32_t off, uint32_t v)
{
    d->regs[off / 4] = v;
}

static inline uint16_t vring_load16(const volatile void *p)
{
    uint16_t v;
    __builtin_memcpy(&v, (const void *)p, sizeof(v));
    return v;
}
static inline void vring_store16(volatile void *p, uint16_t v)
{
    __builtin_memcpy((void *)p, &v, sizeof(v));
}
static inline uint32_t vring_load32(const volatile void *p)
{
    uint32_t v;
    __builtin_memcpy(&v, (const void *)p, sizeof(v));
    return v;
}
static inline void vring_store32(volatile void *p, uint32_t v)
{
    __builtin_memcpy((void *)p, &v, sizeof(v));
}
static inline uint64_t vring_load64(const volatile void *p)
{
    uint64_t v;
    __builtin_memcpy(&v, (const void *)p, sizeof(v));
    return v;
}

// Map [0x0a000000, 0x0a200000) into TTBR1 as Device memory (one 2 MiB
// block) at the direct-map VA so Phy_To_Virt reaches it.  The runtime
// direct map covers the D window only up to 0x0a000000 and nothing else
// claims this VA, so an occupied slot here is a real conflict.
static int map_virtio_window(void)
{
    uint64_t pa  = 0x0a000000UL;
    uint64_t va  = ARCH_PAGE_OFFSET + pa;
    uint32_t perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE;

    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa  = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *root    = (uint64_t *)(uintptr_t)(root_pa + ARCH_PAGE_OFFSET);

    return aarch64_pt_map_2m_block(root, va, pa, perm);
}

// Allocate one zeroed 4 KiB page; returns the VA and records the PA.
static void *alloc_ring_page(uint64_t *pa_out)
{
    uint64_t pa = alloc_4k_page();
    if (!pa) return NULL;
    void *va = Phy_To_Virt((void *)(uintptr_t)pa);
    if (!va) return NULL;
    memset(va, 0, PAGE_4K_SIZE);
    *pa_out = pa;
    return va;
}

static int virtio_blk_read(block_device_t *dev, uint64_t lba, uint32_t count,
                           void *buffer);
static int virtio_blk_write(block_device_t *dev, uint64_t lba, uint32_t count,
                            const void *buffer);
static int virtio_blk_flush(block_device_t *dev);

static const struct block_device_ops virtio_blk_ops = {
    .read  = virtio_blk_read,
    .write = virtio_blk_write,
    .flush = virtio_blk_flush,
};

// Submit one 3-descriptor chain and poll the used ring.  Caller holds
// d->lock; type is VIRTIO_BLK_T_IN (data device-writable) or _OUT.
static int virtio_blk_transfer(struct virtio_blk_dev *d, uint32_t type,
                               uint64_t sector, void *data, uint32_t bytes)
{
    if (!d->ready) return -ENODEV;
    if (bytes > VIRTIO_BLK_MAX_SECTORS * VIRTIO_BLK_SECTOR) return -EINVAL;

    // Scratch buffers: header + status live in their own pages so the
    // device sees stable physical addresses (no stack DMA).
    static uint32_t chain_seq;
    uint32_t chain = chain_seq++ % d->queue_size;
    uint16_t d0 = (uint16_t)(chain * 3), d1 = (uint16_t)(d0 + 1),
             d2 = (uint16_t)(d0 + 2);

    uint8_t *hdr = d->hdr + (size_t)chain * 16;
    uint32_t t   = type;
    uint64_t sec = sector;
    __builtin_memcpy(hdr, &t, 4);
    __builtin_memcpy(hdr + 4, &(uint32_t){0}, 4);
    __builtin_memcpy(hdr + 8, &sec, 8);
    uint64_t hdr_pa = d->hdr_pa + (size_t)chain * 16;

    uint64_t data_pa = Virt_To_Phy(data);
    uint64_t st_pa   = d->status_pa + chain;
    d->status[chain] = 0x99;    // sentinel: device must overwrite it

    volatile struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } *desc
        = (volatile void *)d->desc;
    desc[d0].addr  = hdr_pa;        desc[d0].len  = 16;
    desc[d0].flags = VRING_DESC_F_NEXT; desc[d0].next = d1;
    desc[d1].addr  = data_pa;       desc[d1].len  = bytes;
    desc[d1].flags = (type == VIRTIO_BLK_T_IN) ? VRING_DESC_F_WRITE : 0;
    desc[d1].next  = d2;
    desc[d2].addr  = st_pa;         desc[d2].len  = 1;
    desc[d2].flags = VRING_DESC_F_WRITE;
    desc[d2].next  = 0;

    // avail ring: flags, idx, ring[]
    volatile struct { uint16_t flags, idx, ring[]; } *avail =
        (volatile void *)d->avail;
    uint16_t aidx = avail->idx;
    vring_store16(&avail->ring[aidx % d->queue_size], d0);
    __atomic_store_n(&avail->idx, (uint16_t)(aidx + 1), __ATOMIC_RELEASE);
    w32(d, VMMIO_QUEUE_NOTIFY, 0);

    // Poll the used ring.
    volatile struct { uint16_t flags, idx; struct { uint32_t id, len; } elem[]; } *used =
        (volatile void *)d->used;
    uint32_t spins = 0;
    while (__atomic_load_n(&used->idx, __ATOMIC_ACQUIRE) ==
           (uint16_t)(d->last_used_idx)) {
        if (++spins > VIRTIO_BLK_POLL_LIMIT)
            return -ETIMEDOUT;
        __asm__ volatile("yield");
    }
    // Drain (one chain in flight at a time in practice; loop covers the
    // device batching several completions).
    int rc = -EIO;
    while (used->idx != d->last_used_idx) {
        uint16_t u = d->last_used_idx;
        uint32_t id = vring_load32(&used->elem[u % d->queue_size].id);
        d->last_used_idx = (uint16_t)(u + 1);
        (void)id;
        rc = (d->status[chain] == VIRTIO_BLK_S_OK) ? 0 : -EIO;
    }
    return rc;
}

static int virtio_blk_read(block_device_t *dev, uint64_t lba, uint32_t count,
                           void *buffer)
{
    struct virtio_blk_dev *d = (struct virtio_blk_dev *)dev->private_data;
    if (!d || !d->ready) return -ENODEV;
    if (count == 0 || count > VIRTIO_BLK_MAX_SECTORS) return -EINVAL;
    if (lba + count > dev->sector_count) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&d->lock);
    // Header/status scratch pages are shared across chains, so only one
    // chain may be in flight at a time under this lock.
    int rc = virtio_blk_transfer(d, VIRTIO_BLK_T_IN, lba, buffer,
                                 count * VIRTIO_BLK_SECTOR);
    spin_unlock_irqrestore(&d->lock, flags);
    return rc;
}

static int virtio_blk_write(block_device_t *dev, uint64_t lba, uint32_t count,
                            const void *buffer)
{
    struct virtio_blk_dev *d = (struct virtio_blk_dev *)dev->private_data;
    if (!d || !d->ready) return -ENODEV;
    if (count == 0 || count > VIRTIO_BLK_MAX_SECTORS) return -EINVAL;
    if (lba + count > dev->sector_count) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&d->lock);
    int rc = virtio_blk_transfer(d, VIRTIO_BLK_T_OUT, lba,
                                 (void *)(uintptr_t)buffer,
                                 count * VIRTIO_BLK_SECTOR);
    spin_unlock_irqrestore(&d->lock, flags);
    return rc;
}

static int virtio_blk_flush(block_device_t *dev)
{
    (void)dev;
    // No VIRTIO_BLK_F_FLUSH negotiated: QEMU's virtio-blk-device writes go
    // straight to the host image, so there is no device-side write cache.
    return 0;
}

// Reset a slot's queue registers and start queue 0 with our ring buffers.
static int virtio_blk_setup_queue(struct virtio_blk_dev *d)
{
    uint64_t desc_pa = 0, avail_pa = 0, used_pa = 0, hdr_pa = 0, status_pa = 0;
    void *desc = NULL, *avail = NULL, *used = NULL, *hdr = NULL, *status = NULL;

    w32(d, VMMIO_QUEUE_SEL, 0);
    uint32_t num_max = r32(d, VMMIO_QUEUE_NUM_MAX);
    if (num_max == 0 || num_max == UINT32_MAX) { return -ENODEV; }
    uint32_t qsz = num_max < VIRTIO_BLK_QUEUE_SIZE ? num_max : VIRTIO_BLK_QUEUE_SIZE;
    if (qsz < 3) { return -ENODEV; }

    desc  = alloc_ring_page(&desc_pa);
    avail = alloc_ring_page(&avail_pa);
    used  = alloc_ring_page(&used_pa);
    hdr   = alloc_ring_page(&hdr_pa);
    status = alloc_ring_page(&status_pa);
    if (!desc || !avail || !used || !hdr || !status) goto fail;

    w32(d, VMMIO_QUEUE_NUM, qsz);
    w32(d, VMMIO_QUEUE_DESC_LOW,  (uint32_t)(desc_pa & 0xffffffffu));
    w32(d, VMMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));
    w32(d, VMMIO_QUEUE_AVAIL_LOW,  (uint32_t)(avail_pa & 0xffffffffu));
    w32(d, VMMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail_pa >> 32));
    w32(d, VMMIO_QUEUE_USED_LOW,  (uint32_t)(used_pa & 0xffffffffu));
    w32(d, VMMIO_QUEUE_USED_HIGH, (uint32_t)(used_pa >> 32));
    w32(d, VMMIO_QUEUE_READY, 1);

    d->queue_size = qsz;
    d->desc = desc;  d->desc_pa  = desc_pa;
    d->avail = avail; d->avail_pa = avail_pa;
    d->used = used;  d->used_pa  = used_pa;
    d->hdr = hdr;    d->hdr_pa   = hdr_pa;
    d->status = status; d->status_pa = status_pa;
    d->last_used_idx = 0;
    return 0;

fail:
    if (desc_pa)  free_4k_page(desc_pa);
    if (avail_pa) free_4k_page(avail_pa);
    if (used_pa)  free_4k_page(used_pa);
    if (hdr_pa)   free_4k_page(hdr_pa);
    if (status_pa) free_4k_page(status_pa);
    return -ENOMEM;
}

static int virtio_blk_probe_slot(uint64_t base, uint32_t spi)
{
    if (s_dev_count >= VIRTIO_BLK_MAX_DEVS) return 0;
    struct virtio_blk_dev *d = (struct virtio_blk_dev *)
        calloc(1, sizeof(struct virtio_blk_dev));
    if (!d) return -ENOMEM;

    d->base = base;
    d->regs = (volatile uint32_t *)Phy_To_Virt((void *)(uintptr_t)base);
    spin_init(&d->lock);

    uint32_t magic   = r32(d, VMMIO_MAGIC);
    uint32_t version = r32(d, VMMIO_VERSION);
    uint32_t devid   = r32(d, VMMIO_DEVICE_ID);
    if (magic != 0x74726976u /* 'virt' */) { free(d); return 0; }   // empty slot
    if (version != 2 || devid != 2 /* block */) {
        log_warn("virtio-blk: slot %llx version=%u devid=%u — skipped\n",
                 (unsigned long long)base, version, devid);
        free(d);
        return 0;
    }

    // Reset + acknowledge.
    w32(d, VMMIO_STATUS, 0);
    w32(d, VMMIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);

    // Feature negotiation: accept only VIRTIO_F_VERSION_1 (word 1, bit 0).
    w32(d, VMMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t hi = r32(d, VMMIO_DEVICE_FEATURES);
    if (!(hi & 1u)) {
        log_err("virtio-blk: %llx does not offer VIRTIO_F_VERSION_1\n",
                (unsigned long long)base);
        free(d);
        return 0;
    }
    w32(d, VMMIO_DEVICE_FEATURES_SEL, 0);
    (void)r32(d, VMMIO_DEVICE_FEATURES);        // lo: none accepted
    w32(d, VMMIO_DRIVER_FEATURES_SEL, 0);
    w32(d, VMMIO_DRIVER_FEATURES, 0);
    w32(d, VMMIO_DRIVER_FEATURES_SEL, 1);
    w32(d, VMMIO_DRIVER_FEATURES, 1u);          // VERSION_1

    w32(d, VMMIO_STATUS, r32(d, VMMIO_STATUS) | VIRTIO_S_FEATURES_OK);
    if (!(r32(d, VMMIO_STATUS) & VIRTIO_S_FEATURES_OK)) {
        log_err("virtio-blk: %llx FEATURES_OK refused\n",
                (unsigned long long)base);
        free(d);
        return 0;
    }

    int rc = virtio_blk_setup_queue(d);
    if (rc != 0) {
        log_err("virtio-blk: %llx queue setup failed (%d)\n",
                (unsigned long long)base, rc);
        free(d);
        return rc;
    }

    w32(d, VMMIO_STATUS, r32(d, VMMIO_STATUS) | VIRTIO_S_DRIVER_OK);
    d->ready = true;

    // Config: capacity in 512-byte sectors at config offset 0.
    uint64_t capacity = vring_load64((volatile uint8_t *)d->regs + VMMIO_CONFIG);

    // Stable storage for the registry-visible name (desc.name is not
    // copied by block_device_register).
    static char dev_name[VIRTIO_BLK_MAX_DEVS][8];
    dev_name[s_dev_count][0] = 'h';
    dev_name[s_dev_count][1] = 'd';
    dev_name[s_dev_count][2] = (char)('a' + s_dev_count);
    dev_name[s_dev_count][3] = '\0';

    struct block_device_desc desc = {
        .name         = dev_name[s_dev_count],
        .sector_count = capacity,
        .sector_size  = VIRTIO_BLK_SECTOR,
        .ops          = &virtio_blk_ops,
        .private_data = d,
        .parent       = NULL,
        .kind         = BLOCK_DISK,
    };

    block_device_t *bdev = NULL;
    rc = block_device_register(&desc, &bdev);
    if (rc != 0) {
        log_err("virtio-blk: register %s failed (%d)\n", desc.name, rc);
        d->ready = false;
        free(d);
        return rc;
    }
    s_devs[s_dev_count++] = d;
    log_info("virtio-blk: %s @ %llx sectors=%llu spi=%u\n",
             dev_name[s_dev_count - 1], (unsigned long long)base,
             (unsigned long long)capacity, spi);
    return 0;
}

int virtio_blk_init(void)
{
    static bool initialized;
    if (initialized) return 0;
    initialized = true;

    block_device_init();    // idempotent registry + bcache bring-up

    int rc = map_virtio_window();
    if (rc != 0) {
        log_err("virtio-blk: MMIO window map failed (%d)\n", rc);
        return rc;
    }

    uint32_t n = dtb_virtio_mmio_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct aarch64_virtio_mmio_slot *slot = dtb_virtio_mmio_get(i);
        if (!slot) break;
        int prc = virtio_blk_probe_slot(slot->base, slot->spi);
        if (prc < 0) return prc;
    }
    if (s_dev_count == 0)
        log_warn("virtio-blk: no block device found in %u DTB slots\n", n);
    return 0;
}

// ── Subsys registration (SUBSYS_PHASE_STORAGE, same phase as x86 device-boot)
#ifndef OS01_HOST_TEST
static int virtio_blk_subsys_init(void)
{
    return virtio_blk_init();
}

static int virtio_blk_subsys_register(void)
{
    return register_subsys("virtio-blk", virtio_blk_subsys_init,
                           SUBSYS_PHASE_STORAGE, 0 /* non-OPTIONAL */);
}
SUBSYS_INITCALL(virtio_blk_subsys_register);
#endif
