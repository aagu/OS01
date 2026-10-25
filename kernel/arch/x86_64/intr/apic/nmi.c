#include <intr/apic.h>

// MADT type 4 selects each processor's NMI input (ACPI 6.5 5.2.12.7).
// Unspecified pins remain masked; conforming flags mean active-high/edge.
void lapic_configure_nmi(void)
{
    uint32_t id = lapic_read(LAPIC_ID) >> 24;
    for (uint32_t i = 0; i < apic_info.nmi_count; ++i) {
        const lapic_nmi_t *n = &apic_info.nmis[i];
        int matches = n->acpi_id == 0xff;
        for (uint32_t j = 0; j < apic_info.lapic_count; ++j)
            if (apic_info.lapics[j].apic_id == id &&
                apic_info.lapics[j].acpi_id == n->acpi_id)
                matches = 1;
        uint32_t polarity = n->flags & 3;
        uint32_t trigger = (n->flags >> 2) & 3;
        if (!matches || n->lint > 1 || polarity == 2 || trigger == 2 ||
            (n->flags & ~0xf))
            continue;
        uint32_t value = LVT_DELIVERY_NMI;
        if (polarity == 3) value |= 1u << 13;
        if (trigger == 3) value |= 1u << 15;
        lapic_write(n->lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0, value);
    }
}

