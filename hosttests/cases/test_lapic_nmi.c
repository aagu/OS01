/* Exercise the production MADT parser and routing; substitute MMIO only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/arch/x86_64/intr/apic/acpi.c"

static uint32_t writes[0x400 / 16];

uint32_t lapic_read(uint32_t offset)
{
    (void)offset;
    return 7u << 24;
}

void lapic_write(uint32_t offset, uint32_t value)
{
    writes[offset / 16] = value;
}

static void clear_writes(void)
{
    memset(writes, 0, sizeof(writes));
}

int main(void)
{
    struct {
        struct MADT_HEADER header;
        struct MADT_LAPIC cpu;
        struct MADT_NMI nmi;
    } __attribute__((packed)) table = {
        .header = {.sdt = {.Length = sizeof(table)}},
        .cpu = {MADT_TYPE_LAPIC, sizeof(struct MADT_LAPIC), 42, 7, 1},
        .nmi = {MADT_TYPE_NMI, sizeof(struct MADT_NMI), 255, 0, 1},
    };
    parse_madt(&table.header);
    assert(apic_info.nmi_count == 1);
    assert(apic_info.nmis[0].acpi_id == 255);
    assert(apic_info.nmis[0].lint == 1);
    lapic_configure_nmi();
    assert(writes[LAPIC_LVT_LINT1 / 16] == LVT_DELIVERY_NMI);

    /* Processor UID differs from its APIC ID: only the UID matches. */
    clear_writes();
    apic_info.nmis[0] = (lapic_nmi_t){42, 0, 15};
    lapic_configure_nmi();
    assert(writes[LAPIC_LVT_LINT0 / 16] ==
           (LVT_DELIVERY_NMI | (1 << 13) | (1 << 15)));
    clear_writes();
    apic_info.nmis[0] = (lapic_nmi_t){7, 1, 0};
    lapic_configure_nmi();
    assert(writes[LAPIC_LVT_LINT1 / 16] == 0);

    /* Every flag combination, both inputs, invalid inputs and reserved bits. */
    for (unsigned flags = 0; flags < 32; ++flags) {
        for (unsigned lint = 0; lint < 3; ++lint) {
            clear_writes();
            apic_info.nmis[0] = (lapic_nmi_t){42, lint, flags};
            lapic_configure_nmi();
            unsigned valid = lint < 2 && (flags & 3) != 2 &&
                ((flags >> 2) & 3) != 2 && !(flags & ~15);
            unsigned expected = LVT_DELIVERY_NMI;
            if ((flags & 3) == 3) expected |= 1 << 13;
            if (((flags >> 2) & 3) == 3) expected |= 1 << 15;
            assert(writes[LAPIC_LVT_LINT0 / 16] ==
                   (valid && lint == 0 ? expected : 0));
            assert(writes[LAPIC_LVT_LINT1 / 16] ==
                   (valid && lint == 1 ? expected : 0));
        }
    }
    clear_writes();
    apic_info.lapic_count = 0;
    apic_info.nmis[0] = (lapic_nmi_t){255, 1, 0};
    lapic_configure_nmi();
    assert(writes[LAPIC_LVT_LINT1 / 16] == LVT_DELIVERY_NMI);
    clear_writes();
    apic_info.nmi_count = 0;
    lapic_configure_nmi();
    assert(writes[LAPIC_LVT_LINT1 / 16] == 0);
    /* A truncated type-4 record must not become an enabled route. */
    memset(&apic_info, 0, sizeof(apic_info));
    table.header.sdt.Length = sizeof(table) - sizeof(table.nmi) + 2;
    table.nmi.length = 2;
    parse_madt(&table.header);
    assert(apic_info.nmi_count == 0);
    puts("LAPIC MADT NMI parser and routing: PASS");
    return 0;
}
