#include <kernel/apic.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/asm.h>
#include <stdint.h>
#include <string.h>

// ──────────────────────────────────────────────
//  I/O APIC register access (indirect index/data)
//  base = physical address; caller must have mapped the 2MB region.
// ──────────────────────────────────────────────

static inline void ioapic_write_reg(uint32_t ioapic_phys_base, uint8_t reg, uint32_t value)
{
    volatile uint32_t *regsel = (volatile uint32_t *)Phy_To_Virt(ioapic_phys_base);
    volatile uint32_t *iowin  = (volatile uint32_t *)Phy_To_Virt(ioapic_phys_base + 0x10);

    *regsel = reg;
    *iowin  = value;
}

static inline uint32_t ioapic_read_reg(uint32_t ioapic_phys_base, uint8_t reg)
{
    volatile uint32_t *regsel = (volatile uint32_t *)Phy_To_Virt(ioapic_phys_base);
    volatile uint32_t *iowin  = (volatile uint32_t *)Phy_To_Virt(ioapic_phys_base + 0x10);

    *regsel = reg;
    return *iowin;
}

// ──────────────────────────────────────────────
//  Find the I/O APIC and GSI for a given IRQ vector
// ──────────────────────────────────────────────

// For now we have at most one I/O APIC.  Find the one whose GSI base
// covers the desired GSI, and return the IOAPIC index.
static int find_ioapic_for_gsi(uint32_t gsi, uint8_t *redir_index)
{
    for (uint32_t i = 0; i < apic_info.ioapic_count; i++) {
        ioapic_entry_t *ioapic = &apic_info.ioapics[i];
        uint32_t max_redir = IOAPIC_VER_MAX_REDIR(
            ioapic_read_reg(ioapic->mmio_base, IOAPIC_REG_VER));

        if (gsi >= ioapic->gsi_base && gsi < ioapic->gsi_base + max_redir + 1) {
            *redir_index = (uint8_t)(gsi - ioapic->gsi_base);
            return (int)i;
        }
    }
    return -1;
}

// ──────────────────────────────────────────────
//  I/O APIC → hw_int_controller_t interface
// ──────────────────────────────────────────────

static uint64_t ioapic_install(uint64_t nr, void *arg __attribute__((unused)))
{
    uint32_t isa_irq = (uint32_t)(nr - 0x20);
    uint32_t gsi = isa_irq_to_gsi((uint8_t)isa_irq);

    serial_printk("IOAPIC: install IRQ %u (ISA %u → GSI %u)\n",
                  (unsigned)nr, isa_irq, gsi);

    (void)nr;
    return 0;
}

static void ioapic_uninstall(uint64_t nr)
{
    uint32_t isa_irq = (uint32_t)(nr - 0x20);
    uint32_t gsi = isa_irq_to_gsi((uint8_t)isa_irq);
    uint8_t redir;
    int idx = find_ioapic_for_gsi(gsi, &redir);

    if (idx >= 0) {
        ioapic_entry_t *ioapic = &apic_info.ioapics[idx];
        // Mask the entry
        uint32_t low = ioapic_read_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir));
        ioapic_write_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir), low | IOAPIC_RED_MASK);

        serial_printk("IOAPIC: uninstall IRQ %u (GSI %u)\n", (unsigned)nr, gsi);
    }
}

static void ioapic_enable(uint64_t nr)
{
    uint32_t isa_irq = (uint32_t)(nr - 0x20);
    uint32_t gsi = isa_irq_to_gsi((uint8_t)isa_irq);
    uint8_t redir;
    int idx = find_ioapic_for_gsi(gsi, &redir);

    if (idx < 0) {
        serial_printk("IOAPIC: no I/O APIC found for GSI %u (IRQ %u)\n",
                      gsi, (unsigned)nr);
        return;
    }

    ioapic_entry_t *ioapic = &apic_info.ioapics[idx];

    // Determine polarity and trigger from ISO overrides
    uint32_t low_flags = 0;
    uint8_t iso_pol = ISO_POLARITY_HIGH;
    uint8_t iso_trig = ISO_TRIGGER_EDGE;

    for (uint32_t i = 0; i < apic_info.iso_count; i++) {
        if (apic_info.isos[i].irq_source == isa_irq) {
            iso_pol  = apic_info.isos[i].flags & ISO_POLARITY_MASK;
            iso_trig = apic_info.isos[i].flags & ISO_TRIGGER_MASK;
            break;
        }
    }

    // Polarity
    if (iso_pol == ISO_POLARITY_LOW || iso_pol == ISO_POLARITY_LOW2)
        low_flags |= IOAPIC_RED_POL_LOW;

    // Trigger mode
    if (iso_trig == ISO_TRIGGER_LEVEL)
        low_flags |= IOAPIC_RED_TRIG_LEVEL;

    // Vector number = IRQ vector (0x20 + isa_irq), physical destination mode
    uint32_t vector = (uint32_t)nr;
    low_flags |= vector;   // bits 0-7: interrupt vector
    low_flags |= IOAPIC_RED_DEL_FIXED;    // fixed delivery mode (value 0)

    // Destination: BSP LAPIC ID 0, physical mode
    uint32_t high = 0;   // destination = 0 (BSP APIC ID), physical mode

    // Write the redirection entries
    ioapic_write_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir) + 1, high);
    ioapic_write_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir), low_flags);

    serial_printk("IOAPIC: enable IRQ %u (GSI %u, redir %u) → vector %#x, low=%#x\n",
                  (unsigned)nr, gsi, redir, (unsigned)vector, low_flags);
}

static void ioapic_disable(uint64_t nr)
{
    uint32_t isa_irq = (uint32_t)(nr - 0x20);
    uint32_t gsi = isa_irq_to_gsi((uint8_t)isa_irq);
    uint8_t redir;
    int idx = find_ioapic_for_gsi(gsi, &redir);

    if (idx < 0)
        return;

    ioapic_entry_t *ioapic = &apic_info.ioapics[idx];
    uint32_t low = ioapic_read_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir));
    ioapic_write_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(redir), low | IOAPIC_RED_MASK);

    serial_printk("IOAPIC: disable IRQ %u (GSI %u)\n", (unsigned)nr, gsi);
}

static void ioapic_ack(uint64_t nr __attribute__((unused)))
{
    // Acknowledge via LAPIC EOI register
    lapic_eoi();
}

// ──────────────────────────────────────────────
//  The I/O APIC controller instance
// ──────────────────────────────────────────────

hw_int_controller_t ioapic_controller = {
    .enable    = ioapic_enable,
    .disable   = ioapic_disable,
    .install   = ioapic_install,
    .uninstall = ioapic_uninstall,
    .ack       = ioapic_ack,
};

hw_int_controller_t * get_ioapic_controller(void)
{
    return &ioapic_controller;
}

// ──────────────────────────────────────────────
//  Initialization
// ──────────────────────────────────────────────

int ioapic_init(void)
{
    if (apic_info.ioapic_count == 0) {
        serial_printk("IOAPIC: no I/O APICs found in MADT\n");
        return 0;
    }

    for (uint32_t i = 0; i < apic_info.ioapic_count; i++) {
        ioapic_entry_t *ioapic = &apic_info.ioapics[i];

        // Map the I/O APIC's 2MB MMIO region
        uint64_t ioapic_page = ioapic->mmio_base & PAGE_2M_MASK;
        vmm_map_page(kernel_map, ioapic_page,
                     (uintptr_t)Phy_To_Virt(ioapic_page), PAGE_KERNEL_MMIO);

        // Read version to get maximum redirection entry
        uint32_t ver = ioapic_read_reg(ioapic->mmio_base, IOAPIC_REG_VER);
        uint32_t max_redir = IOAPIC_VER_MAX_REDIR(ver);

        serial_printk("IOAPIC: id=%u base=%#010x version=%#x max_redir=%u\n",
                      ioapic->apic_id, ioapic->mmio_base, ver, max_redir);

        // Mask all redirection entries initially
        for (uint32_t n = 0; n <= max_redir; n++) {
            uint32_t low = ioapic_read_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(n));
            ioapic_write_reg(ioapic->mmio_base, IOAPIC_REG_REDTBL(n), low | IOAPIC_RED_MASK);
        }

        serial_printk("IOAPIC: %u redirection entries masked\n", max_redir + 1);
    }

    return 1;
}
