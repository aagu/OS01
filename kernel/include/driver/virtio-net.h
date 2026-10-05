// kernel/include/driver/virtio-net.h — VirtIO-net driver for OS01
#ifndef _DRIVER_VIRTIO_NET_H
#define _DRIVER_VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/spinlock.h>
#include <net/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

#ifndef OS01_HOST_TEST
#include <lwip/netif.h>
#endif

#include <arch/cpu.h>
#include <intr/interrupt.h>

struct Page;

// ── PCI IDs ─────────────────────────────────────────────────────
#define VIRTIO_PCI_VENDOR_ID             0x1AF4
#define VIRTIO_PCI_DEVICE_ID_NET_MODERN  0x1041  // modern only
#define VIRTIO_PCI_DEVICE_ID_NET_LEGACY  0x1000  // legacy/transitional

// ── Legacy virtio register offsets (from BAR0 IO base) ──────────
#define VIRTIO_LEGACY_HOST_FEATURES   0x00
#define VIRTIO_LEGACY_GUEST_FEATURES  0x04
#define VIRTIO_LEGACY_QUEUE_PFN       0x08
#define VIRTIO_LEGACY_QUEUE_SIZE      0x0C
#define VIRTIO_LEGACY_QUEUE_SELECT    0x0E
#define VIRTIO_LEGACY_QUEUE_NOTIFY    0x10
#define VIRTIO_LEGACY_DEVICE_STATUS   0x12
#define VIRTIO_LEGACY_ISR_STATUS      0x13

// ── Device Status bits ──────────────────────────────────────────
#define VIRTIO_STATUS_ACKNOWLEDGE        (1 << 0)
#define VIRTIO_STATUS_DRIVER             (1 << 1)
#define VIRTIO_STATUS_FAILED             (1 << 7)
#define VIRTIO_STATUS_FEATURES_OK        (1 << 3)
#define VIRTIO_STATUS_DRIVER_OK          (1 << 2)
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET (1 << 6)

// ── Feature bits ────────────────────────────────────────────────
#define VIRTIO_F_VERSION_1          (1ULL << 32)
#define VIRTIO_NET_F_MAC            (1ULL << 5)   // device provides MAC
#define VIRTIO_NET_F_STATUS         (1ULL << 16)  // link status reporting

// ── Queue indices ───────────────────────────────────────────────
#define VIRTIO_NET_RX_QUEUE         0
#define VIRTIO_NET_TX_QUEUE         1

// ── ISR bits ────────────────────────────────────────────────────
#define VIRTIO_ISR_QUEUE_INTR     (1 << 0)
#define VIRTIO_ISR_DEVICE_INTR    (1 << 1)

// ── Virtqueue descriptor flags ──────────────────────────────────
#define VIRTQ_DESC_F_NEXT         (1 << 0)
#define VIRTQ_DESC_F_WRITE        (1 << 1)
#define VIRTQ_DESC_F_INDIRECT     (1 << 2)

// ── Virtqueue "used" ring flags ─────────────────────────────────
#define VIRTQ_USED_F_NO_NOTIFY    (1 << 0)

// ── Descriptor table entry (16 bytes) ───────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

// ── Available ring ──────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

// ── Used ring element ───────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

// ── Used ring ───────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

// ── Virtqueue structure ─────────────────────────────────────────
typedef struct {
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    uint16_t       size;
    uint16_t       last_used_idx;
} virtq_t;

// ── Virtio-net packet header ────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} virtio_net_hdr_t;

#define VIRTIO_NET_VQ_SIZE     256
#define VIRTIO_NET_RX_BUF_SIZE 2048
#define VIRTIO_NET_HDR_SIZE    10

// ── Driver instance state ───────────────────────────────────────
struct virtio_net_instance {
    struct pci_device *pdev;
    uint16_t           io_base;
    uint8_t            mac[6];

    // Net device abstraction
    struct net_device *ndev;

    // Interrupt & mode
    enum nic_irq_mode  irq_mode;
    uint32_t           gsi;
    bool               irq_owned;
    bool               legacy_mode;

    // Queues and memory
    virtq_t            rx_vq;
    virtq_t            tx_vq;
    uint16_t           rx_qsize;
    uint16_t           tx_qsize;
    struct Page       *rx_page;
    struct Page       *tx_page;
    uint64_t           rx_phys;
    uint64_t           tx_phys;

    // Packet buffers
    uint64_t           rx_buf_phys[VIRTIO_NET_VQ_SIZE];
    void              *rx_bufs[VIRTIO_NET_VQ_SIZE];
    uint64_t           tx_buf_phys[VIRTIO_NET_VQ_SIZE];

    // TX tracking
    uint16_t           tx_desc_head;
    uint16_t           tx_desc_tail;
    spinlock_T         tx_lock;

    // Legacy support
    struct netif      *netif_ptr;

    int                initialized;
    bool               stopped;
};

// ── Driver API ──────────────────────────────────────────────────
int   virtio_net_probe(struct pci_device *pdev, const struct pci_device_id *id);
void  virtio_net_remove(struct pci_device *pdev);
extern const struct pci_driver virtio_net_pci_driver;

// ── Legacy transitional API ─────────────────────────────────────
int   virtio_net_legacy_init(struct pci_device *pdev, uint64_t bar, uint8_t gsi);
int   virtio_net_init(uint64_t bar_phys, uint8_t bus, uint8_t dev, uint8_t func, uint8_t gsi);
err_t virtio_netif_init(struct netif *netif);
void  virtio_net_poll_rx(void);
void  virtio_net_handler(uint64_t nr, uint64_t param, pt_regs_t *regs);

#endif // _DRIVER_VIRTIO_NET_H
