#ifndef _DRIVER_AHCI_H
#define _DRIVER_AHCI_H

#include <stdint.h>

// ────────────────────────────────────────────────────────────
//  AHCI register and structure definitions
//  Based on the OSDev wiki AHCI article and Intel AHCI spec rev 1.3.1
// ────────────────────────────────────────────────────────────

// ── Global HBA registers (ABAR + 0x00) ────────────────────

typedef volatile struct {
    uint32_t cap;          // 0x00 - Host Capability
    uint32_t ghc;          // 0x04 - Global Host Control
    uint32_t is;           // 0x08 - Interrupt Status
    uint32_t pi;           // 0x0C - Ports Implemented
    uint32_t vs;           // 0x10 - Version
    uint32_t ccc_ctl;      // 0x14 - Command Completion Coalescing Control
    uint32_t ccc_pts;      // 0x18 - Command Completion Coalescing Ports
    uint32_t em_loc;       // 0x1C - Enclosure Management Location
    uint32_t em_ctl;       // 0x20 - Enclosure Management Control
    uint32_t cap2;         // 0x24 - Host Capabilities Extended
    uint32_t bohc;         // 0x28 - BIOS/OS Handoff Control & Status
    uint8_t  rsv[0xA0 - 0x2C];          // 0x2C – 0x9F
    uint8_t  vendor[0x100 - 0xA0];      // 0xA0 – 0xFF
    // Port registers start at 0x100 — accessed via ahci_port()
} HBA_MEM;

// ── Per-port registers (each 0x80 bytes, at ABAR + 0x100 + n*0x80) ──

typedef volatile struct {
    uint32_t clb;          // 0x00 - Command List Base (lower)
    uint32_t clbu;         // 0x04 - Command List Base (upper)
    uint32_t fb;           // 0x08 - FIS Base (lower)
    uint32_t fbu;          // 0x0C - FIS Base (upper)
    uint32_t is;           // 0x10 - Interrupt Status
    uint32_t ie;           // 0x14 - Interrupt Enable
    uint32_t cmd;          // 0x18 - Command & Status
    uint32_t rsv0;         // 0x1C
    uint32_t tfd;          // 0x20 - Task File Data
    uint32_t sig;          // 0x24 - Signature
    uint32_t ssts;         // 0x28 - SATA Status (SCR0)
    uint32_t sctl;         // 0x2C - SATA Control (SCR2)
    uint32_t serr;         // 0x30 - SATA Error (SCR1)
    uint32_t sact;         // 0x34 - SATA Active (SCR3)
    uint32_t ci;           // 0x38 - Command Issue
    uint32_t sntf;         // 0x3C - SATA Notification (SCR4)
    uint32_t fbs;          // 0x40 - FIS-based Switching Control
    uint32_t rsv1[11];     // 0x44 – 0x6F
    uint32_t vendor[4];    // 0x70 – 0x7F
} HBA_PORT;

// Helper macro: get a pointer to port N
#define AHCI_PORT(hba, n)  ((HBA_PORT *)((uint8_t *)(hba) + 0x100 + (n) * 0x80))

// ── GHC (Global Host Control) bits ──────────────────────
#define AHCI_GHC_AE      (1U << 31)   // AHCI Enable
#define AHCI_GHC_IE      (1U << 1)    // Interrupt Enable
#define AHCI_GHC_HR      (1U << 0)    // HBA Reset (write 1, wait for 0)

// ── CAP (Capability) bits ───────────────────────────────
#define AHCI_CAP_NP(cap)      ((cap) & 0x1F)         // Number of Ports
#define AHCI_CAP_S64A(cap)    (((cap) >> 31) & 1)    // 64-bit Addressing
#define AHCI_CAP_SSS(cap)     (((cap) >> 27) & 1)    // Staggered Spin-up
#define AHCI_CAP_SAM(cap)     (((cap) >> 29) & 1)    // Supports AHCI mode only

// ── CAP2 (Capabilities Extended) ───────────────────────
#define AHCI_CAP2_BOH(cap2)   (((cap2) >> 0) & 1)    // BIOS/OS Handoff

// ── BOHC (BIOS/OS Handoff Control) ─────────────────────
#define AHCI_BOHC_BOS  (1U << 0)    // BIOS Own Semaphore
#define AHCI_BOHC_OOS  (1U << 1)    // OS Own Semaphore
#define AHCI_BOHC_SOOE (1U << 2)    // SMI on OS Ownership Change
#define AHCI_BOHC_OOC  (1U << 3)    // OS Ownership Change

// ── Port CMD (Command & Status) bits ───────────────────
#define AHCI_PORT_CMD_ST     (1U << 0)   // Start (port run)
#define AHCI_PORT_CMD_SUD    (1U << 1)   // Spin-Up Device
#define AHCI_PORT_CMD_POD    (1U << 2)   // Power On Device
#define AHCI_PORT_CMD_CLO    (1U << 3)   // Command List Override
#define AHCI_PORT_CMD_FRE    (1U << 4)   // FIS Receive Enable
#define AHCI_PORT_CMD_CCS    (1U << 5)   // Current Command Slot (bit 5-7)
#define AHCI_PORT_CMD_MPSS   (1U << 13)  // Mechanical Presence Switch
#define AHCI_PORT_CMD_FR     (1U << 14)  // FIS Receive Running
#define AHCI_PORT_CMD_CR     (1U << 15)  // Command List Running
#define AHCI_PORT_CMD_CPD    (1U << 16)  // Cold Presence Detection
#define AHCI_PORT_CMD_ESP    (1U << 17)  // External SATA Port
#define AHCI_PORT_CMD_FBSCP  (1U << 18)  // FIS-based Switching Capable Port
#define AHCI_PORT_CMD_APSTE  (1U << 19)  // Automatic Partial to Slumber
#define AHCI_PORT_CMD_ATAPI  (1U << 24)  // Device is ATAPI
#define AHCI_PORT_CMD_DLAE   (1U << 25)  // Drive LED on ATAPI
#define AHCI_PORT_CMD_ALPE   (1U << 26)  // Aggressive Link Power Mgmt
#define AHCI_PORT_CMD_ASP    (1U << 27)  // Aggressive Slumber / Partial
#define AHCI_PORT_CMD_ICC_MASK (0xF << 28) // Interface Communication Control

// ── Port SSTS (SATA Status) bits ────────────────────────
#define AHCI_PORT_SSTS_DET_MASK    0x0F        // Device Detection
#define AHCI_PORT_SSTS_DET_NODEV   0x00        // No device detected
#define AHCI_PORT_SSTS_DET_PRES    0x03        // Device present & communication established
#define AHCI_PORT_SSTS_IPM_MASK    0xF00       // Interface Power Management
#define AHCI_PORT_SSTS_IPM_ACTIVE  0x100       // Active state

// ── Port SCTL (SATA Control) bits ──────────────────────
#define AHCI_PORT_SCTL_DET_MASK     0x0F
#define AHCI_PORT_SCTL_DET_NONE     0x00   // No action
#define AHCI_PORT_SCTL_DET_INIT     0x01   // Perform COMRESET
#define AHCI_PORT_SCTL_DET_DISABLE  0x04   // Disable device

// ── Port SERR (SATA Error) bits ────────────────────────
// Write 1 to clear; all bits defined in spec; clear all with 0xFFFFFFFF

// ── Port IS (Interrupt Status) bits ────────────────────
#define AHCI_PORT_IS_DHRS  (1U << 0)   // Device to Host Register FIS
#define AHCI_PORT_IS_PSS   (1U << 1)   // PIO Setup FIS
#define AHCI_PORT_IS_DSS   (1U << 2)   // DMA Setup FIS
#define AHCI_PORT_IS_SDBS  (1U << 3)   // Set Device Bits FIS
#define AHCI_PORT_IS_UFS   (1U << 4)   // Unknown FIS
#define AHCI_PORT_IS_DPS   (1U << 5)   // Descriptor Processed
#define AHCI_PORT_IS_PCS   (1U << 6)   // Port Connect Change
#define AHCI_PORT_IS_DMP   (1U << 7)   // Device Mechanical Presence
#define AHCI_PORT_IS_PRCS  (1U << 22)  // PhyRdy Change Status
#define AHCI_PORT_IS_OF    (1U << 24)  // Overflow
#define AHCI_PORT_IS_IPMS  (1U << 25)  // Incorrect Port Multiplier
#define AHCI_PORT_IS_HBD   (1U << 26)  // Host Bus Data Error
#define AHCI_PORT_IS_HBF   (1U << 27)  // Host Bus Fatal Error (DMAR)
#define AHCI_PORT_IS_TFES  (1U << 30)  // Task File Error Status
#define AHCI_PORT_IS_CPDS  (1U << 31)  // Cold Presence Detect

// ── Command Header (32 bytes each, 32 slots per port) ──

typedef struct {
    uint8_t  cfl:5;          // Command FIS length in DWORDS (2-16)
    uint8_t  a:1;            // ATAPI
    uint8_t  w:1;            // Write (1=H2D, 0=D2H)
    uint8_t  p:1;            // Prefetchable

    uint8_t  r:1;            // Reset
    uint8_t  b:1;            // BIST
    uint8_t  c:1;            // Clear Busy Upon R_OK
    uint8_t  rsv0:1;

    uint8_t  pmp:4;          // Port Multiplier Port
    uint16_t prdtl;          // Physical Region Descriptor Table Length

    volatile uint32_t prdbc; // PRD Byte Count Transferred

    uint32_t ctba;           // Command Table Base Address (lower)
    uint32_t ctbau;          // Command Table Base Address (upper)

    uint32_t rsv1[4];        // Reserved
} __attribute__((packed)) HBA_CMD_HEADER;

// ── PRDT Entry (16 bytes) ────────────────────────────────

typedef struct {
    uint32_t dba;            // Data Base Address (lower)
    uint32_t dbau;           // Data Base Address (upper)
    uint32_t rsv0;           // Reserved

    uint32_t dbc:22;         // Data Byte Count (0-based, so n-1)
    uint32_t rsv1:9;         // Reserved
    uint32_t i:1;            // Interrupt on Completion
} __attribute__((packed)) HBA_PRDT_ENTRY;

// ── Command Table (128-byte aligned) ─────────────────────

typedef struct {
    uint8_t  cfis[64];       // Command FIS (0x00 – 0x3F)
    uint8_t  acmd[16];       // ATAPI command (0x40 – 0x4F)
    uint8_t  rsv[48];        // Reserved (0x50 – 0x7F)
    // PRDT entries follow at offset 0x80
    HBA_PRDT_ENTRY prdt_entry[];
} __attribute__((packed)) HBA_CMD_TBL;

// ── Received FIS Area (256-byte aligned) ─────────────────

typedef volatile struct {
    uint8_t  dsfis[32];      // DMA Setup FIS (0x00)
    uint8_t  pad0[4];

    uint8_t  psfis[32];      // PIO Setup FIS (0x20)
    uint8_t  pad1[12];

    uint8_t  rfis[32];       // D2H Register FIS (0x40)
    uint8_t  pad2[4];

    uint8_t  sdbfis[8];      // Set Device Bits FIS (0x58)
    uint8_t  ufis[64];       // Unknown FIS (0x60)
    uint8_t  rsv[0x100 - 0xA0];
} HBA_FIS;

// ── FIS types ─────────────────────────────────────────────

#define FIS_TYPE_REG_H2D      0x27   // Register FIS – Host to Device
#define FIS_TYPE_REG_D2H      0x34   // Register FIS – Device to Host
#define FIS_TYPE_DMA_ACT      0x39   // DMA Activate
#define FIS_TYPE_DMA_SETUP    0x41   // DMA Setup
#define FIS_TYPE_DATA         0x46   // Data FIS
#define FIS_TYPE_PIO_SETUP    0x5F   // PIO Setup
#define FIS_TYPE_DEV_BITS     0xA1   // Set Device Bits

// ── ATA commands ─────────────────────────────────────────

#define ATA_CMD_IDENTIFY          0xEC
#define ATA_CMD_READ_DMA_EXT      0x25
#define ATA_CMD_WRITE_DMA_EXT     0x35
#define ATA_CMD_READ_SECTORS      0x20
#define ATA_CMD_WRITE_SECTORS     0x30
#define ATA_CMD_FLUSH_CACHE       0xE7
#define ATA_CMD_FLUSH_CACHE_EXT   0xEA
#define ATA_CMD_SET_FEATURES      0xEF

// ── Identify Device word offsets ─────────────────────────

#define ATA_IDENT_MODEL           27     // words 27-46, 20 ASCII chars
#define ATA_IDENT_SERIAL          10     // words 10-19, 10 ASCII chars
#define ATA_IDENT_FW_REV          23     // words 23-26, 4 ASCII chars
#define ATA_IDENT_LBA28_SECTORS   60     // words 60-61, 28-bit LBA
#define ATA_IDENT_LBA48_SECTORS   100    // words 100-103, 48-bit LBA

// ── Driver API ───────────────────────────────────────────

void ahci_init(void);

// Block read/write — returns 0 on success, -1 on error
int ahci_read_sectors(int port_num, uint64_t lba, uint32_t count, void *buffer);
int ahci_write_sectors(int port_num, uint64_t lba, uint32_t count, const void *buffer);

// Query port info
int  ahci_port_present(int port_num);
uint64_t ahci_port_sector_count(int port_num);

#endif // _DRIVER_AHCI_H
