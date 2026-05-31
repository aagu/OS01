#include <driver/pci.h>
#include <kernel/arch/x86_64/hw.h>
#include <kernel/printk.h>
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
    outd(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    return ind(PCI_CONFIG_DATA);
}

void pci_config_write(uint32_t bus, uint32_t dev, uint32_t func, uint32_t offset, uint32_t value)
{
    outd(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    outd(PCI_CONFIG_DATA, value);
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
    serial_printk("PCI: scanning for class=%02x subclass=%02x progIF=%02x\n",
                   class_code, subclass, prog_if);
    int ret = scan_bus(0, class_code, subclass, prog_if, out_bus, out_dev, out_func);
    if (ret == 0) {
        serial_printk("PCI: found at %d:%d.%d\n", *out_bus, *out_dev, *out_func);
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

    serial_printk("PCI: enabled bus mastering for %d:%d.%d\n", bus, dev, func);
}

void pci_enable_mmio(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_config_read(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write(bus, dev, func, PCI_COMMAND, cmd);

    serial_printk("PCI: enabled MMIO for %d:%d.%d\n", bus, dev, func);
}

// ── Interrupt line ────────────────────────────────────────

uint8_t pci_read_interrupt_line(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t reg = pci_config_read(bus, dev, func, PCI_INTERRUPT_LINE);
    return (uint8_t)(reg & 0xFF);
}
