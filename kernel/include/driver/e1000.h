// kernel/driver/e1000.h — Intel 82540EM (e1000) register definitions
#ifndef _DRIVER_E1000_H
#define _DRIVER_E1000_H

#include <stdint.h>

// ── Register offsets (16-byte aligned for 64-bit MMIO) ────────
#define E1000_REG_CTRL     0x0000   // Device Control
#define E1000_REG_CTRL_EXT 0x0018   // Extended Device Control
#define E1000_REG_STATUS   0x0008   // Device Status
#define E1000_REG_EERD     0x0014   // EEPROM Read
#define E1000_REG_RAL0     0x5400   // RX Address Low 0 (MAC[3:0] — auto-loaded on 82574L)
#define E1000_REG_RAH0     0x5404   // RX Address High 0 (MAC[5:4] + AV bit)
#define E1000_REG_ICR      0x00C0   // Interrupt Cause Read (R/W1C)
#define E1000_REG_ICS      0x00C8   // Interrupt Cause Set (WO)
#define E1000_REG_IMS      0x00D0   // Interrupt Mask Set
#define E1000_REG_IMC      0x00D8   // Interrupt Mask Clear
#define E1000_REG_IVAR     0x00E4   // Interrupt Vector Allocation (82574 MSI-X)
#define E1000_REG_RCTL     0x0100   // Receive Control
#define E1000_REG_TCTL     0x0400   // Transmit Control
#define E1000_REG_RDBAL    0x2800   // RX Descriptor Base Low
#define E1000_REG_RDBAH    0x2804   // RX Descriptor Base High
#define E1000_REG_RDLEN    0x2808   // RX Descriptor Length
#define E1000_REG_RDH      0x2810   // RX Descriptor Head
#define E1000_REG_RDT      0x2818   // RX Descriptor Tail
#define E1000_REG_RDTR     0x2820   // RX Delay Timer
#define E1000_REG_RADV     0x282C   // RX Absolute Delay Timer
#define E1000_REG_TDBAL    0x3800   // TX Descriptor Base Low
#define E1000_REG_TDBAH    0x3804   // TX Descriptor Base High
#define E1000_REG_TDLEN    0x3808   // TX Descriptor Length
#define E1000_REG_TDH      0x3810   // TX Descriptor Head
#define E1000_REG_TDT      0x3818   // TX Descriptor Tail

// ── CTRL_EXT bits ─────────────────────────────────────────────
#define E1000_CTRL_EXT_INT_MODE (1 << 30)   // 0 = INTx, 1 = MSI

// ── CTRL bits ─────────────────────────────────────────────────
#define E1000_CTRL_FD       (1 << 0)
#define E1000_CTRL_ASDE     (1 << 5)
#define E1000_CTRL_SLU      (1 << 6)
#define E1000_CTRL_ILOS     (1 << 7)
#define E1000_CTRL_RST      (1 << 26)

// ── STATUS bits ────────────────────────────────────────────────
#define E1000_STATUS_FD     (1 << 0)
#define E1000_STATUS_LU     (1 << 1)

// ── EERD bits ──────────────────────────────────────────────────
#define E1000_EERD_START    (1 << 0)
#define E1000_EERD_DONE     (1 << 4)
#define E1000_EERD_DATA_MASK 0xFFFF0000
#define E1000_EERD_DATA_SHIFT 16

// ── RCTL bits ──────────────────────────────────────────────────
#define E1000_RCTL_EN       (1 << 1)
#define E1000_RCTL_SBP      (1 << 2)
#define E1000_RCTL_UPE      (1 << 3)
#define E1000_RCTL_MPE      (1 << 4)
#define E1000_RCTL_LPE      (1 << 5)
#define E1000_RCTL_LBM_NONE 0
#define E1000_RCTL_LBM_LOOP (3 << 6)
#define E1000_RCTL_RDMTS_HALF 0
#define E1000_RCTL_RDMTS_QUARTER (1 << 8)
#define E1000_RCTL_RDMTS_EIGHTH (2 << 8)
#define E1000_RCTL_BAM      (1 << 15)
#define E1000_RCTL_BSIZE_256   (3 << 16)
#define E1000_RCTL_BSIZE_512   (2 << 16)
#define E1000_RCTL_BSIZE_1024  (1 << 16)
#define E1000_RCTL_BSIZE_2048  0
#define E1000_RCTL_BSIZE_4096  ((3 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_8192  ((2 << 16) | (1 << 25))
#define E1000_RCTL_BSIZE_16384 ((1 << 16) | (1 << 25))
#define E1000_RCTL_VFE      (1 << 18)
#define E1000_RCTL_SECRC    (1 << 26)

// ── TCTL bits ──────────────────────────────────────────────────
#define E1000_TCTL_EN       (1 << 1)
#define E1000_TCTL_PSP      (1 << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_COLD_FULLDUPLEX 0x40
#define E1000_TCTL_COLD_HALFDUPLEX 0x200

// ── ICR / IMS bits ─────────────────────────────────────────────
#define E1000_ICR_TXDW      (1 << 0)
#define E1000_ICR_TXQE      (1 << 1)
#define E1000_ICR_LSC       (1 << 2)
#define E1000_ICR_RXSEQ     (1 << 3)
#define E1000_ICR_RXDMT0    (1 << 4)
#define E1000_ICR_RXO       (1 << 6)
#define E1000_ICR_RXT0      (1 << 7)
// 82574L (e1000e) MSI-X per-queue causes.  QEMU's e1000e raises
// RXQ0 (not RXT0) for received packets when MSI-X is enabled.
#define E1000_ICR_RXQ0      (1 << 20)
#define E1000_ICR_TXQ0      (1 << 22)
#define E1000_ICR_OTHER     (1 << 24)

// ── RX descriptor ──────────────────────────────────────────────
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32

typedef struct {
    uint64_t addr;       // physical address of data buffer
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

#define E1000_RXD_STAT_DD  (1 << 0)   // Descriptor Done
#define E1000_RXD_STAT_EOP (1 << 1)   // End of Packet

// ── TX descriptor ──────────────────────────────────────────────
typedef struct {
    uint64_t addr;       // physical address of data buffer
    uint16_t length;
    uint8_t  cso;        // Checksum Offset
    uint8_t  cmd;        // Command
    uint8_t  status;     // Status (written by hardware on completion)
    uint8_t  css;        // Checksum Start
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

#define E1000_TXD_CMD_EOP  (1 << 0)   // End of Packet
#define E1000_TXD_CMD_IFCS (1 << 1)   // Insert FCS/CRC
#define E1000_TXD_CMD_RS   (1 << 3)   // Report Status
#define E1000_TXD_STAT_DD  (1 << 0)   // Descriptor Done

#include <stdbool.h>
#include <arch/spinlock.h>
#include <net/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>

#ifndef OS01_HOST_TEST
#include "lwip/netif.h"  // for struct netif, struct pbuf, err_t
#endif

struct Page;

#define E1000_RXQ_DEPTH  64

typedef struct {
    uint8_t  *buf[E1000_RXQ_DEPTH];
    uint16_t  len[E1000_RXQ_DEPTH];
    int       head;   // next slot to fill
    int       tail;   // next slot to drain
} e1000_rxq_t;

struct e1000_instance {
    struct pci_device *pdev;
    uint64_t           mmio_phys;
    volatile uint8_t  *mmio;           // kernel-virtual MMIO base
    uint8_t            mac[6];

    // Net device abstraction
    struct net_device *ndev;

    // Interrupt & mode
    enum nic_irq_mode  irq_mode;
    uint32_t           gsi;
    bool               irq_owned;
    bool               legacy_mode;

    // Descriptors and DMA
    e1000_rx_desc_t   *rx_descs;
    e1000_tx_desc_t   *tx_descs;
    uint64_t           rx_phys;
    uint64_t           tx_phys;
    struct Page       *rx_page;
    struct Page       *tx_page;

    // DMA packet buffers
    uint64_t           rx_buf_phys[E1000_NUM_RX_DESC];
    uint64_t           tx_buf_phys[E1000_NUM_TX_DESC];
    uint8_t           *rx_bufs[E1000_NUM_RX_DESC];
    uint8_t           *tx_bufs[E1000_NUM_TX_DESC];

    // TX state
    uint32_t           tx_head;       // next descriptor to send
    uint32_t           tx_tail;       // next free slot (post-completion)
    spinlock_T         tx_lock;

    // RX state
    uint32_t           rx_tail;       // next descriptor to check
    e1000_rxq_t        rxq;

    // Legacy support
    struct netif      *netif_ptr;

    int                initialized;
    bool               stopped;
};

// ── Driver API ─────────────────────────────────────────────────
int   e1000_probe(struct pci_device *pdev, const struct pci_device_id *id);
void  e1000_remove(struct pci_device *pdev);
extern const struct pci_driver e1000_pci_driver;

int   e1000_legacy_init(struct pci_device *pdev, uint64_t bar, uint8_t gsi, int use_msi);
int   e1000_init(uint64_t bar_phys, uint8_t gsi, int use_msi);
int   e1000_link_up(void);
err_t e1000_xmit(struct netif *netif, struct pbuf *p);
err_t e1000_netif_init(struct netif *netif);

void e1000_poll_rx(void);
void e1000_process_rx(void);

#endif // _DRIVER_E1000_H
