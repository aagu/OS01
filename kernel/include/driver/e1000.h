// kernel/driver/e1000.h — Intel 82540EM (e1000) register definitions
#ifndef _DRIVER_E1000_H
#define _DRIVER_E1000_H

#include <stdint.h>

// ── Register offsets (16-byte aligned for 64-bit MMIO) ────────
#define E1000_REG_CTRL     0x0000   // Device Control
#define E1000_REG_STATUS   0x0008   // Device Status
#define E1000_REG_EERD     0x0014   // EEPROM Read
#define E1000_REG_ICR      0x00C0   // Interrupt Cause Read
#define E1000_REG_IMS      0x00D0   // Interrupt Mask Set
#define E1000_REG_IMC      0x00D8   // Interrupt Mask Clear
#define E1000_REG_RCTL     0x0100   // Receive Control
#define E1000_REG_TCTL     0x0400   // Transmit Control
#define E1000_REG_RDBAL    0x2800   // RX Descriptor Base Low
#define E1000_REG_RDBAH    0x2804   // RX Descriptor Base High
#define E1000_REG_RDLEN    0x2808   // RX Descriptor Length
#define E1000_REG_RDH      0x2810   // RX Descriptor Head
#define E1000_REG_RDT      0x2818   // RX Descriptor Tail
#define E1000_REG_RDTR     0x2820   // RX Delay Timer
#define E1000_REG_TDBAL    0x3800   // TX Descriptor Base Low
#define E1000_REG_TDBAH    0x3804   // TX Descriptor Base High
#define E1000_REG_TDLEN    0x3808   // TX Descriptor Length
#define E1000_REG_TDH      0x3810   // TX Descriptor Head
#define E1000_REG_TDT      0x3818   // TX Descriptor Tail
#define E1000_REG_RA_BASE  0x5400   // Receive Address (MAC) Filter

// ── CTRL bits ─────────────────────────────────────────────────
#define E1000_CTRL_FD       (1 << 0)
#define E1000_CTRL_ASDE     (1 << 5)
#define E1000_CTRL_SLU      (1 << 6)
#define E1000_CTRL_ILOS     (1 << 7)
#define E1000_CTRL_SPEED_10  0
#define E1000_CTRL_SPEED_100 (1 << 8)
#define E1000_CTRL_SPEED_1000 (2 << 8)
#define E1000_CTRL_SPEED_MASK (3 << 8)
#define E1000_CTRL_FRCSPD   (1 << 11)
#define E1000_CTRL_FRCDPLX  (1 << 12)
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

// ── Driver API ─────────────────────────────────────────────────
#include "lwip/netif.h"  // for struct netif, struct pbuf, err_t

int   e1000_init(uint64_t bar_phys, uint8_t irq);
err_t e1000_xmit(struct netif *netif, struct pbuf *p);
err_t e1000_netif_init(struct netif *netif);

#endif // _DRIVER_E1000_H
