#ifndef _DRIVER_PCI_H
#define _DRIVER_PCI_H

#include <stdint.h>

// ── PCI configuration space access ─────────────────────────
// Standard legacy access via I/O ports 0xCF8 / 0xCFC

uint32_t pci_config_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset);
void     pci_config_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset, uint32_t value);
void     pci_config_writeb(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset, uint8_t value);

// ── Device discovery ───────────────────────────────────────
// Returns 0 on success, -1 if not found.
int pci_find_device(uint8_t class_code, uint8_t subclass,
                    uint8_t prog_if,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func);

// ── BAR helpers ────────────────────────────────────────────
// Reads and decodes a BAR, returning the base address.
// Sets is_mmio (non-zero if MMIO BAR, zero if I/O BAR) and is_64bit.
uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func,
                      uint32_t bar_index, int *is_mmio, int *is_64bit);

// ── Command register helpers ───────────────────────────────
void pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func);
void pci_enable_mmio(uint8_t bus, uint8_t dev, uint8_t func);

// ── Interrupt routing / MSI ────────────────────────────────
uint8_t pci_read_interrupt_line(uint8_t bus, uint8_t dev, uint8_t func);
uint8_t pci_get_gsi(uint8_t bus, uint8_t dev, uint8_t func);
void   pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t func);
// Enable MSI at the given vector.  Returns 0 on success, -1 if not MSI-capable.
int    pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector);

// ── Standard PCI header offsets ────────────────────────────
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS_CODE      0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_SECONDARY_BUS   0x19   // for bridge header type 1
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// ── Command register bits ──────────────────────────────────
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEM_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)

// ── Header types ───────────────────────────────────────────
#define PCI_HEADER_DEVICE    0
#define PCI_HEADER_BRIDGE    1

// ── MSI capability structure ───────────────────────────────
#define PCI_CAP_ID_MSI       0x05
#define PCI_MSI_ADDR_BASE    0xFEE00000   // target LAPIC (physical, dest=0)
#define PCI_MSI_FLAGS_OFF    2            // Message Control u16 offset in cap
#define PCI_MSI_FLAGS_64BIT  (1 << 7)     // 64-bit addressing capable
#define PCI_MSI_ADDR_LO_OFF  4            // Message Address low u32 offset
#define PCI_MSI_ADDR_HI_OFF  8            // Message Address high u32 (64-bit)
#define PCI_MSI_DATA_32_OFF  8            // Message Data u16 offset (32-bit)
#define PCI_MSI_DATA_64_OFF  12           // Message Data u16 offset (64-bit)

// ── Class codes ────────────────────────────────────────────
#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_SUBCLASS_SATA        0x06
#define PCI_PROGIF_AHCI          0x01

#define PCI_CLASS_NETWORK          0x02
#define PCI_SUBCLASS_ETHERNET      0x00

#endif // _DRIVER_PCI_H
