// kernel/include/driver/virtio-net.h — VirtIO-net driver for OS01
//
// Implements modern (v1.0) VirtIO transport via PCI MMIO.
// Reference: Virtual I/O Device (VIRTIO) Version 1.2
// QEMU: -device virtio-net-pci,netdev=net0
//
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <lwip/netif.h>

// ── PCI IDs ─────────────────────────────────────────────────────
#define VIRTIO_PCI_VENDOR_ID    0x1AF4
#define VIRTIO_PCI_DEVICE_ID_NET_MODERN  0x1041  // modern only
#define VIRTIO_PCI_DEVICE_ID_NET_LEGACY  0x1000  // legacy/transitional

// ── PCI Capability types (cfg_type field) ───────────────────────
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

// ── Device Status bits ──────────────────────────────────────────
#define VIRTIO_STATUS_ACKNOWLEDGE        (1 << 0)
#define VIRTIO_STATUS_DRIVER             (1 << 1)
#define VIRTIO_STATUS_FAILED             (1 << 7)
#define VIRTIO_STATUS_FEATURES_OK        (1 << 3)
#define VIRTIO_STATUS_DRIVER_OK          (1 << 2)
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET (1 << 6)

// ── Feature bits (selected, relevant for virtio-net) ────────────
#define VIRTIO_F_VERSION_1          (1ULL << 32)  // modern transport
#define VIRTIO_NET_F_MAC            (1ULL << 5)   // device provides MAC
#define VIRTIO_NET_F_STATUS         (1ULL << 16)  // link status reporting

// ── Queue indices ───────────────────────────────────────────────
#define VIRTIO_NET_RX_QUEUE         0
#define VIRTIO_NET_TX_QUEUE         1

// ── Common config register offsets (within common cfg BAR+offset) ─
#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT   0x00  // u32
#define VIRTIO_COMMON_DEVICE_FEATURE          0x04  // u32
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT   0x08  // u32
#define VIRTIO_COMMON_DRIVER_FEATURE          0x0C  // u32
#define VIRTIO_COMMON_MSIX_CONFIG             0x10  // u16
#define VIRTIO_COMMON_NUM_QUEUES              0x12  // u16
#define VIRTIO_COMMON_DEVICE_STATUS           0x14  // u8
#define VIRTIO_COMMON_CONFIG_GENERATION       0x15  // u8
#define VIRTIO_COMMON_QUEUE_SELECT            0x16  // u16
#define VIRTIO_COMMON_QUEUE_SIZE              0x18  // u16
#define VIRTIO_COMMON_QUEUE_MSIX_VECTOR       0x1A  // u16
#define VIRTIO_COMMON_QUEUE_ENABLE            0x1C  // u16
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF        0x1E  // u16
#define VIRTIO_COMMON_QUEUE_DESC              0x20  // u64
#define VIRTIO_COMMON_QUEUE_DRIVER            0x28  // u64 (avail ring)
#define VIRTIO_COMMON_QUEUE_DEVICE            0x30  // u64 (used ring)

// ── Device config offset (net-specific, within device cfg BAR+offset) ─
#define VIRTIO_NET_CONFIG_MAC                 0x00  // 6 bytes
#define VIRTIO_NET_CONFIG_STATUS              0x06  // u16
// VIRTIO_NET_S_LINK_UP = 1

// ── ISR bits ────────────────────────────────────────────────────
#define VIRTIO_ISR_QUEUE_INTR     (1 << 0)
#define VIRTIO_ISR_DEVICE_INTR    (1 << 1)

// ── Virtqueue descriptor flags ──────────────────────────────────
#define VIRTQ_DESC_F_NEXT         (1 << 0)   // has next descriptor
#define VIRTQ_DESC_F_WRITE        (1 << 1)   // device-writable buffer
#define VIRTQ_DESC_F_INDIRECT     (1 << 2)   // indirect table

// ── Virtqueue "used" ring flags ─────────────────────────────────
#define VIRTQ_USED_F_NO_NOTIFY    (1 << 0)   // suppress notifications

// ── Descriptor table entry (16 bytes) ───────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t addr;    // guest-physical address of buffer
    uint32_t len;     // buffer length
    uint16_t flags;   // VIRTQ_DESC_F_*
    uint16_t next;    // index of next descriptor (if F_NEXT)
} virtq_desc_t;

// ── Available ring (driver → device) ────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;   // VIRTQ_USED_F_NO_NOTIFY
    uint16_t idx;     // driver updates this index
    uint16_t ring[];  // descriptor indices (size: queue_size)
    // uint16_t used_event;  // (only if VIRTIO_F_EVENT_IDX)
} virtq_avail_t;

// ── Used ring element (device → driver) ─────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t id;      // descriptor index
    uint32_t len;     // bytes written
} virtq_used_elem_t;

// ── Used ring (device → driver) ─────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;   // VIRTQ_USED_F_NO_NOTIFY
    uint16_t idx;     // device updates this index
    virtq_used_elem_t ring[];  // (size: queue_size)
    // uint16_t avail_event;   // (only if VIRTIO_F_EVENT_IDX)
} virtq_used_t;

// ── Virtqueue structure ─────────────────────────────────────────
typedef struct {
    virtq_desc_t  *desc;       // contiguous descriptor array (virtq_size)
    virtq_avail_t *avail;      // available ring
    virtq_used_t  *used;       // used ring
    uint16_t       size;       // max descriptors
    uint16_t       last_used_idx;  // driver's last-seen used index
    // RX-only: array of buffer physical addresses
    uint64_t      *rx_bufs_phys;
    void         **rx_bufs;    // virtual pointers (for recycling)
} virtq_t;

// ── Virtio-net packet header ────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  flags;          // VIRTIO_NET_HDR_F_NEEDS_CSUM, etc.
    uint8_t  gso_type;       // GSO type (0=none)
    uint16_t hdr_len;        // header length for GSO
    uint16_t gso_size;       // GSO segment size
    uint16_t csum_start;     // checksum calculation start offset
    uint16_t csum_offset;    // checksum field offset within header
    uint16_t num_buffers;    // only with MRG_RXBUF
} virtio_net_hdr_t;

// ── Driver state ────────────────────────────────────────────────
struct virtio_net_dev {
    // MMIO base pointers (kernel virtual)
    volatile uint8_t *common;    // common config MMIO
    volatile uint8_t *notify;    // notify MMIO
    volatile uint8_t *isr;       // ISR status MMIO
    volatile uint8_t *device;    // device-specific config MMIO

    uint32_t notify_off_mult;    // queue_notify_off multiplier

    uint8_t  mac[6];
    uint8_t  irq;
    uint32_t irq_vector;

    virtq_t  rx_vq;
    virtq_t  tx_vq;

    struct netif *netif_ptr;     // set during netif_add

    // Buffer info
    uint32_t buf_size;           // size of each RX buffer (2048)
};

// ── Public API ──────────────────────────────────────────────────
int   virtio_net_init(uint64_t bar_phys, uint8_t bus, uint8_t dev, uint8_t func);
err_t virtio_netif_init(struct netif *netif);
void  virtio_net_poll_rx(void);

