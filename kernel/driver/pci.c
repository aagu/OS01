#include <driver/pci.h>
#include <kernel/arch/io.h>
#include <kernel/debug.h>
#include <stdint.h>

// ── Legacy PCI config space access via 0xCF8 / 0xCFC ─────

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

// Configuration address format:
//   bit 31    = enable (must be 1)
//   bits 30-24= reserved (0)
//   bits 23-16= bus number
//   bits 15-11= device number
//   bits 10-8 = function number
//   bits 7-2  = register offset (dword aligned)
//   bits 1-0  = 0
static uint32_t pci_make_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset)
{
    return 0x80000000U
         | ((bus  & 0xFF) << 16)
         | ((dev  & 0x1F) << 11)
         | ((func & 0x07) << 8)
         | (offset & 0xFC);
}

uint32_t pci_config_read(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset)
{
    arch_outd(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    return arch_ind(PCI_CONFIG_DATA);
}

void pci_config_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset, uint32_t value)
{
    arch_outd(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    arch_outd(PCI_CONFIG_DATA, value);
}

void pci_config_writeb(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset, uint8_t value)
{
    arch_outd(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    arch_outb(value, PCI_CONFIG_DATA + (offset & 3));
}

// ── Device discovery ──────────────────────────────────────

// Check if a function exists (vendor != 0xFFFF)
static int pci_device_exists(uint32_t bus, uint32_t dev, uint32_t func)
{
    uint32_t vendor = pci_config_read(bus, dev, func, PCI_VENDOR_ID) & 0xFFFF;
    return vendor != 0xFFFF;
}

// Scan all functions of a device; returns the first match.
// For multi-function devices (header type bit 7 set), scans func 0-7.
// For single-function, just checks func 0.
static int scan_device(uint32_t bus, uint32_t dev,
                       uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                       uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func)
{
    // Always scan all 8 functions — some devices (e.g. Q35 ISA/LPC bridge
    // at 0:31.0) don't set the multi-function bit but have multiple functions.
    for (uint32_t func = 0; func < 8; func++) {
        if (!pci_device_exists(bus, dev, func))
            continue;

        uint32_t class = pci_config_read(bus, dev, func, PCI_CLASS_CODE);
        uint8_t cc  = (class >> 24) & 0xFF;
        uint8_t sc  = (class >> 16) & 0xFF;
        uint8_t pif = (class >> 8)  & 0xFF;

        if (cc == class_code && sc == subclass
            && (prog_if == 0xFF || pif == prog_if)) {
            *out_bus  = (uint8_t)bus;
            *out_dev  = (uint8_t)dev;
            *out_func = (uint8_t)func;
            return 0;
        }
    }
    return -1;
}

// Scan a bus: iterate devices 0-31, check headers.
// For PCI-PCI bridges, recurse into the secondary bus.
static int scan_bus(uint32_t bus,
                    uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        if (!pci_device_exists(bus, dev, 0))
            continue;

        int ret = scan_device(bus, dev, class_code, subclass, prog_if,
                              out_bus, out_dev, out_func);
        if (ret == 0)
            return 0;

        // Check if this is a PCI-PCI bridge → recurse
        uint32_t hdr = pci_config_read(bus, dev, 0, PCI_HEADER_TYPE);
        if ((hdr & 0x7F) == PCI_HEADER_BRIDGE) {
            uint32_t sec_bus = (pci_config_read(bus, dev, 0, PCI_SECONDARY_BUS) >> 8) & 0xFF;
            if (sec_bus != 0) {
                ret = scan_bus(sec_bus, class_code, subclass, prog_if,
                               out_bus, out_dev, out_func);
                if (ret == 0)
                    return 0;
            }
        }
    }
    return -1;
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func)
{
    debug_block("PCI: scanning for class=%02x subclass=%02x progIF=%02x\n",
                   class_code, subclass, prog_if);
    int ret = scan_bus(0, class_code, subclass, prog_if, out_bus, out_dev, out_func);
    if (ret == 0) {
        debug_block("PCI: found at %d:%d.%d\n", *out_bus, *out_dev, *out_func);
    }
    return ret;
}

// ── BAR reading ───────────────────────────────────────────

uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func,
                      uint32_t bar_index, int *is_mmio, int *is_64bit)
{
    uint32_t offset = PCI_BAR0 + bar_index * 4;
    uint32_t low = pci_config_read(bus, dev, func, offset);

    if (is_mmio)   *is_mmio  = !(low & 1);   // bit 0 = 0 → MMIO, 1 → I/O
    if (is_64bit)  *is_64bit = 0;

    if (low & 1) {
        // I/O BAR
        return low & 0xFFFFFFFC;
    }

    // MMIO BAR
    uint32_t type = (low >> 1) & 0x3;
    if (type == 0) {
        // 32-bit MMIO
        return low & 0xFFFFFFF0;
    }

    // 64-bit MMIO (type == 2): read upper 32 bits
    if (is_64bit) *is_64bit = 1;
    uint32_t high = pci_config_read(bus, dev, func, offset + 4);
    return ((uint64_t)high << 32) | (low & 0xFFFFFFF0);
}

// ── Command register ──────────────────────────────────────

void pci_enable_bus_mastering(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_config_read(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write(bus, dev, func, PCI_COMMAND, cmd);

    debug_block("PCI: enabled bus mastering for %d:%d.%d\n", bus, dev, func);
}

void pci_enable_mmio(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_config_read(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write(bus, dev, func, PCI_COMMAND, cmd);

    debug_block("PCI: enabled MMIO for %d:%d.%d\n", bus, dev, func);
}

// ── MSI disable ────────────────────────────────────────────
// Walk the PCI capabilities list and clear the MSI Enable bit.
// Required for devices that default to MSI on Q35 — without this,
// INTx is never asserted and the IOAPIC never fires.
void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cap_reg = pci_config_read(bus, dev, func, 0x34);
    uint8_t cap_ptr = (uint8_t)(cap_reg & 0xFF);

    while (cap_ptr != 0) {
        uint8_t base = cap_ptr & 0xFC;
        uint8_t off  = cap_ptr & 3;

        uint32_t dword = pci_config_read(bus, dev, func, base);
        uint8_t cap_id = (uint8_t)((dword >> (off * 8)) & 0xFF);

        if (cap_id == 0x05) {  // MSI capability
            uint8_t mc_off = off + 2;   // Message Control at cap_ptr+2
            uint16_t msg_ctrl = (uint16_t)((dword >> (mc_off * 8)) & 0xFFFF);
            if (msg_ctrl & 1) {
                msg_ctrl &= ~1;
                uint32_t mask = 0xFFFF << (mc_off * 8);
                dword = (dword & ~mask) | ((uint32_t)msg_ctrl << (mc_off * 8));
                pci_config_write(bus, dev, func, base, dword);
                log_info("PCI: disabled MSI for %d:%d.%d\n", bus, dev, func);
            }
            return;
        }
        cap_ptr = (uint8_t)((dword >> ((off + 1) * 8)) & 0xFF);
    }
}

// ── MSI enable ─────────────────────────────────────────────
// Walk the PCI capabilities list, find the MSI capability, and
// configure it to deliver edge-triggered fixed-mode interrupts
// at the given vector to LAPIC ID 0 (BSP).
// Returns 0 on success, -1 if no MSI capability found.
int pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector)
{
    uint32_t cap_reg = pci_config_read(bus, dev, func, 0x34);
    uint8_t cap_ptr = (uint8_t)(cap_reg & 0xFF);

    log_info("PCI: %d:%d.%d caps_ptr=%#x\n", bus, dev, func, cap_ptr);
    if (cap_ptr == 0) {
        log_info("PCI: %d:%d.%d has no capabilities\n", bus, dev, func);
    }
    while (cap_ptr != 0) {
        // cap_ptr is always dword-aligned in practice — read full dword
        uint32_t dword = pci_config_read(bus, dev, func, cap_ptr);
        uint8_t cap_id = (uint8_t)(dword & 0xFF);
        log_info("PCI: %d:%d.%d cap_id=%#x\n", bus, dev, func, cap_id);
        if (cap_id == PCI_CAP_ID_MSI) {
            uint16_t msg_ctrl = (uint16_t)(dword >> 16);
            int is_64 = (msg_ctrl & PCI_MSI_FLAGS_64BIT) ? 1 : 0;

            // Write Message Address (4 bytes at cap_ptr + 4)
            pci_config_write(bus, dev, func, cap_ptr + 4, PCI_MSI_ADDR_BASE);

            if (is_64)
                pci_config_write(bus, dev, func, cap_ptr + 8, 0);

            // Write Message Data (u16 at cap_ptr + 8 for 32-bit, +12 for 64-bit)
            uint8_t data_off = is_64 ? 12 : 8;
            uint32_t data_dword = pci_config_read(bus, dev, func, cap_ptr + data_off);
            data_dword = (data_dword & 0xFFFF0000) | vector;
            pci_config_write(bus, dev, func, cap_ptr + data_off, data_dword);

            // Enable MSI: set bit 0 of Message Control (bit 16 of dword)
            pci_config_write(bus, dev, func, cap_ptr, dword | (1 << 16));

                log_info("PCI: MSI enabled for %d:%d.%d vector=0x%x (%d-bit)\n",
                     bus, dev, func, vector, is_64 ? 64 : 32);
            return 0;
        }
        cap_ptr = (uint8_t)(dword >> 8);
    }
    return -1;
}

// ── MSI-X enable ──────────────────────────────────────────────
// Finds the MSI-X capability, configures entry 0 with the given
// vector, and enables MSI-X.  Bypasses IOAPIC entirely.
// Returns 0 on success, -1 if no MSI-X capability found.
int pci_enable_msix(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector)
{
    uint32_t cap_reg = pci_config_read(bus, dev, func, 0x34);
    uint8_t cap_ptr = (uint8_t)(cap_reg & 0xFF);

    if (cap_ptr == 0) {
        log_info("PCI: %d:%d.%d no caps\n", bus, dev, func);
    }

    while (cap_ptr != 0) {
        uint32_t dword = pci_config_read(bus, dev, func, cap_ptr);
        uint8_t cap_id = (uint8_t)(dword & 0xFF);

        if (cap_id == 0x11) {  // MSI-X capability
            uint16_t msg_ctrl = (uint16_t)(dword >> 16);
            uint32_t table_size = (msg_ctrl & 0x7FF) + 1;

            // Read Table BIR and offset
            uint32_t table_reg = pci_config_read(bus, dev, func, cap_ptr + 4);
            uint8_t bir = (uint8_t)(table_reg & 0x7);
            uint32_t tbl_off = table_reg & ~0x7U;

            // Get BAR base address (must be MMIO)
            int is_mmio, is_64bit;
            uint64_t bar_phys = pci_read_bar(bus, dev, func, bir, &is_mmio, &is_64bit);
            if (!is_mmio) {
                log_info("PCI: %d:%d.%d MSI-X table BAR is not MMIO\n", bus, dev, func);
                return -1;
            }

            // BSP LAPIC ID for MSI-X destination
            extern uint32_t lapic_read(uint32_t offset);
            uint32_t bsp_lapic_id = (lapic_read(0x020) >> 24) & 0xFF;

            // Map the MSI-X table for CPU access
            uint64_t table_phys = bar_phys + tbl_off;
            uint64_t page_base = table_phys & 0xFFFFFFFFFFE00000ULL; // 2MB align

            
            // Use the identity-mapped page table - table is within the first 32MB
            // which is already identity-mapped by the kernel, so no explicit map needed.

            volatile uint32_t *entry = (volatile uint32_t *)(table_phys + 0xFFFF800000000000ULL);

            // Configure entry 0 (we use the first vector)
            entry[0] = 0xFEE00000 | (bsp_lapic_id << 12);  // Message Address
            entry[1] = 0;                                    // Upper Address = 0
            entry[2] = (uint32_t)vector;                     // Message Data
            entry[3] = 0;                                    // Unmask

            // Enable MSI-X: set bit 15 of Message Control
            pci_config_write(bus, dev, func, cap_ptr + 2,
                             (pci_config_read(bus, dev, func, cap_ptr + 2) & 0xFFFF0000) | 0x8000);

            log_info("PCI: MSI-X enabled for %d:%d.%d vector=0x%x (table_size=%u bir=%u tbl_off=%#x)\n",
                     bus, dev, func, vector, (unsigned)table_size, bir, tbl_off);
            return 0;
        }
        cap_ptr = (uint8_t)(pci_config_read(bus, dev, func, cap_ptr) >> 8);
    }
    return -1;
}
// Returns the GSI (Global System Interrupt) for a PCI device.
//
// On Q35/ICH9, PCI INTx is routed through PIRQ[A-D] to GSIs 16-19:
//   PIRQ = (slot + pin - 1) & 3,  GSI = 16 + PIRQ
//
// The PCI "interrupt line" register (offset 0x3C) contains a legacy ISA IRQ
// value (0-15) for PIC compatibility, NOT the actual GSI.  Using it with the
// IOAPIC programs the wrong redirection entry → interrupt never delivered.

uint8_t pci_read_interrupt_line(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t reg = pci_config_read(bus, dev, func, PCI_INTERRUPT_LINE);
    return (uint8_t)(reg & 0xFF);
}

uint8_t pci_get_gsi(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t reg = pci_config_read(bus, dev, func, PCI_INTERRUPT_LINE);
    uint8_t int_line = (uint8_t)(reg & 0xFF);
    uint8_t int_pin  = (uint8_t)((reg >> 8) & 0xFF);

    if (int_pin == 0)
        return int_line;  // MSI or no interrupt

    // Compute GSI using Q35/ICH9 routing:
    //   PIRQ[A-D] → GSI[16-19],  PIRQ = (slot + pin - 1) & 3
    uint8_t gsi = 16 + ((dev + int_pin - 1) & 3);

    // If firmware already set interrupt line to a value >= 16, trust it
    // (OVMF on some configs writes the GSI directly).
    if (int_line >= 16)
        return int_line;

    return gsi;
}
