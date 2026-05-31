#include <kernel/apic.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/printk.h>
#include <stdint.h>
#include <string.h>

// ──────────────────────────────────────────────
//  ACPI structures (packed — these come from firmware)
// ──────────────────────────────────────────────

// RSDP: Root System Description Pointer
struct RSDP {
    char     Signature[8];   // "RSD PTR "
    uint8_t  Checksum;
    char     OEMID[6];
    uint8_t  Revision;
    uint32_t RsdtAddress;
    // If Revision >= 2:
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t  ExtendedChecksum;
    uint8_t  Reserved[3];
} __attribute__((packed));

// SDT: System Description Table header (common to RSDT/XSDT/MADT/etc.)
struct SDT_HEADER {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OEMID[6];
    char     OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__((packed));

// MADT entry header
struct MADT_ENTRY {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

// MADT type 0: Processor Local APIC
struct MADT_LAPIC {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));

// MADT type 1: I/O APIC
struct MADT_IOAPIC {
    uint8_t  type;
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed));

// MADT type 2: Interrupt Source Override
struct MADT_ISO {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus_source;
    uint8_t  irq_source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed));

// MADT type 4: Local APIC NMI
struct MADT_NMI {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_processor_id;
    uint16_t flags;
    uint8_t  lint;
} __attribute__((packed));

// Wrapper for the MADT (SDT header + MADT-specific fields)
struct MADT_HEADER {
    struct SDT_HEADER sdt;
    uint32_t LocalApicAddress;   // typically 0xFEE00000
    uint32_t Flags;              // bit 0 = PC-AT dual 8259 (PIC present)
    // Followed by variable-length MADT entries...
} __attribute__((packed));

// ──────────────────────────────────────────────
//  Global APIC info populated by ACPI parsing
// ──────────────────────────────────────────────

apic_info_t apic_info;

// ──────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────

// Verify SDT checksum (all bytes sum to 0)
static int sdt_checksum_valid(void *table, uint32_t length)
{
    uint8_t sum = 0;
    uint8_t *p = (uint8_t *)table;
    for (uint32_t i = 0; i < length; i++)
        sum += p[i];
    return sum == 0;
}

// Ensure a physical address is mapped with 2MB MMIO page
// Falls within the first 32MB identity map (no-op), otherwise maps explicitly.
static void ensure_mapped(uint64_t phys_addr)
{
    uint64_t base = phys_addr & PAGE_2M_MASK;
    // The initial page tables map physical 0-32MB via both identity and
    // higher-half.  For anything above 32MB, explicitly request an MMIO
    // mapping so Phy_To_Virt() works.
    if (base >= 32UL * 1024 * 1024) {
        uintptr_t virt = (uintptr_t)Phy_To_Virt(base);
        // Check if already mapped (non-zero PDE entry)
        // We just call vmm_map_page unconditionally; it's idempotent for the
        // same physical->virtual mapping since get_next_level reuses existing
        // table entries.
        vmm_map_page(kernel_map, base, virt, PAGE_KERNEL_MMIO);
    }
}

// ──────────────────────────────────────────────
//  MADT parser
// ──────────────────────────────────────────────

static void parse_madt(struct MADT_HEADER *madt)
{
    apic_info.lapic_base = madt->LocalApicAddress;
    uint32_t table_length = madt->sdt.Length;
    uint8_t *ptr = (uint8_t *)madt + sizeof(struct MADT_HEADER);
    uint8_t *end = (uint8_t *)madt + table_length;

    serial_printk("APIC: MADT at %p, length=%u, LAPIC base=%#010lx, flags=%#x\n",
                  madt, table_length, apic_info.lapic_base, madt->Flags);

    while (ptr < end) {
        struct MADT_ENTRY *entry = (struct MADT_ENTRY *)ptr;

        if (entry->length == 0)
            break;   // safety

        switch (entry->type) {

        case MADT_TYPE_LAPIC: {
            struct MADT_LAPIC *lapic = (struct MADT_LAPIC *)ptr;
            if (apic_info.lapic_count < MAX_LAPICS) {
                apic_info.lapics[apic_info.lapic_count].apic_id = lapic->apic_id;
                apic_info.lapics[apic_info.lapic_count].acpi_id = lapic->acpi_processor_id;
                apic_info.lapics[apic_info.lapic_count].flags   = lapic->flags;
                serial_printk("APIC:   LAPIC id=%u acpi=%u flags=%#x\n",
                              lapic->apic_id, lapic->acpi_processor_id, lapic->flags);
                apic_info.lapic_count++;
            }
            break;
        }

        case MADT_TYPE_IOAPIC: {
            struct MADT_IOAPIC *ioapic = (struct MADT_IOAPIC *)ptr;
            if (apic_info.ioapic_count < MAX_IOAPICS) {
                apic_info.ioapics[apic_info.ioapic_count].apic_id   = ioapic->ioapic_id;
                apic_info.ioapics[apic_info.ioapic_count].mmio_base = ioapic->ioapic_address;
                apic_info.ioapics[apic_info.ioapic_count].gsi_base  = ioapic->global_system_interrupt_base;
                serial_printk("APIC:   IOAPIC id=%u addr=%#010x gsi_base=%u\n",
                              ioapic->ioapic_id, ioapic->ioapic_address,
                              ioapic->global_system_interrupt_base);
                apic_info.ioapic_count++;
            }
            break;
        }

        case MADT_TYPE_ISO: {
            struct MADT_ISO *iso = (struct MADT_ISO *)ptr;
            if (apic_info.iso_count < MAX_ISO_OVERRIDES) {
                apic_info.isos[apic_info.iso_count].bus_source = iso->bus_source;
                apic_info.isos[apic_info.iso_count].irq_source = iso->irq_source;
                apic_info.isos[apic_info.iso_count].gsi        = iso->global_system_interrupt;
                apic_info.isos[apic_info.iso_count].flags      = iso->flags;
                serial_printk("APIC:   ISO bus=%u irq=%u → GSI=%u flags=%#x\n",
                              iso->bus_source, iso->irq_source,
                              iso->global_system_interrupt, iso->flags);
                apic_info.iso_count++;
            }
            break;
        }

        case MADT_TYPE_NMI: {
            struct MADT_NMI *nmi = (struct MADT_NMI *)ptr;
            serial_printk("APIC:   NMI acpi_processor=%u lint=%u flags=%#x\n",
                          nmi->acpi_processor_id, nmi->lint, nmi->flags);
            break;
        }

        default:
            serial_printk("APIC:   Unknown MADT entry type=%u len=%u\n",
                          entry->type, entry->length);
            break;
        }

        ptr += entry->length;
    }
}

// ──────────────────────────────────────────────
//  Public API: parse ACPI tables from RSDP
// ──────────────────────────────────────────────

int acpi_parse(uint64_t rsdp_phys)
{
    if (!rsdp_phys) {
        serial_printk("APIC: RSDP is NULL — no ACPI tables available\n");
        return 0;
    }

    // Map the RSDP's 2MB region
    ensure_mapped(rsdp_phys);

    struct RSDP *rsdp = (struct RSDP *)Phy_To_Virt(rsdp_phys);

    // Validate signature
    if (memcmp(rsdp->Signature, "RSD PTR ", 8) != 0) {
        serial_printk("APIC: RSDP signature mismatch\n");
        return 0;
    }

    // Validate checksum
    if (!sdt_checksum_valid(rsdp, 20)) {
        serial_printk("APIC: RSDP checksum invalid\n");
        return 0;
    }

    serial_printk("APIC: RSDP found, Revision=%u\n", rsdp->Revision);

    // Prefer XSDT (64-bit pointers) when available, otherwise RSDT (32-bit)
    uint64_t sdt_phys;
    int use_xsdt = 0;

    if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
        sdt_phys  = rsdp->XsdtAddress;
        use_xsdt  = 1;
    } else {
        sdt_phys  = rsdp->RsdtAddress;
    }

    // Map the SDT region
    ensure_mapped(sdt_phys);

    struct SDT_HEADER *sdt = (struct SDT_HEADER *)Phy_To_Virt(sdt_phys);

    if (!sdt_checksum_valid(sdt, sdt->Length)) {
        serial_printk("APIC: %s checksum invalid\n", use_xsdt ? "XSDT" : "RSDT");
        return 0;
    }

    // Count entry pointers
    uint32_t entry_count = (sdt->Length - sizeof(struct SDT_HEADER))
                           / (use_xsdt ? 8 : 4);

    serial_printk("APIC: %s at %p, %u entries\n",
                  use_xsdt ? "XSDT" : "RSDT", sdt, entry_count);

    // Walk entries looking for MADT
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t entry_phys;

        if (use_xsdt) {
            uint64_t *xsdt_entries = (uint64_t *)((uint8_t *)sdt + sizeof(struct SDT_HEADER));
            entry_phys = xsdt_entries[i];
        } else {
            uint32_t *rsdt_entries = (uint32_t *)((uint8_t *)sdt + sizeof(struct SDT_HEADER));
            entry_phys = rsdt_entries[i];
        }

        ensure_mapped(entry_phys);

        struct SDT_HEADER *table = (struct SDT_HEADER *)Phy_To_Virt(entry_phys);

        // Verify checksum before accessing Length
        if (!sdt_checksum_valid(table, table->Length)) {
            serial_printk("APIC:   table at %p checksum invalid, skipping\n", table);
            continue;
        }

        if (memcmp(table->Signature, "APIC", 4) == 0) {
            parse_madt((struct MADT_HEADER *)table);
            return 1;   // success
        }
    }

    serial_printk("APIC: MADT not found in ACPI tables\n");
    return 0;
}

// ──────────────────────────────────────────────
//  ISO lookup: ISA IRQ → GSI (with overrides)
// ──────────────────────────────────────────────

uint32_t isa_irq_to_gsi(uint8_t isa_irq)
{
    // Check for ISO overrides
    for (uint32_t i = 0; i < apic_info.iso_count; i++) {
        if (apic_info.isos[i].irq_source == isa_irq)
            return apic_info.isos[i].gsi;
    }
    // Default: ISA IRQ N → GSI N
    return isa_irq;
}
