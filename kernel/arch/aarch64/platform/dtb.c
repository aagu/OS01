/* UEFI-only DTB handoff wrapper. Pure parsing lives in dtb_parse.c. */
#include <arch/aarch64/acpi.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/pcie.h>
#include <arch/aarch64/rp1_xhci.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/device_window.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/cpu.h>
#include <core/bootinfo.h>
#include <stddef.h>

void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

_Static_assert(AARCH64_BOOT_MAX_CPUS == NR_CPUS, "DTB/CPU capacity mismatch");

static struct aarch64_platform_info platform;
static struct aarch64_pcie_topology s_pcie_topology;
static struct aarch64_rp1_xhci_desc s_rp1_xhci_desc;
static struct aarch64_rp1_irq_desc s_rp1_irq_desc;

const struct aarch64_pcie_topology *aarch64_pcie_topology_get(void)
{
    return &s_pcie_topology;
}

const struct aarch64_rp1_xhci_desc *aarch64_rp1_xhci_desc_get(void)
{
    return &s_rp1_xhci_desc;
}

const struct aarch64_rp1_irq_desc *aarch64_rp1_irq_desc_get(void)
{
    return &s_rp1_irq_desc;
}

const struct aarch64_topology *dtb_topology(void) { return &platform.topology; }
uint32_t dtb_cpu_count(void) { return platform.topology.cpu_count; }
uint64_t dtb_gicd_base(void) { return platform.gicd_base; }
uint64_t dtb_gicc_base(void) { return platform.gicc_base; }
uint64_t dtb_gicr_base(void) { return platform.gicr_base; }
uint8_t dtb_gic_version(void) { return platform.gic_version; }
uint64_t dtb_pl011_base(void) { return platform.pl011_base; }
uint32_t dtb_pl011_spi(void) { return platform.pl011_spi; }
uint32_t dtb_pl011_clock(void) { return platform.pl011_clock; }
uint32_t dtb_cntp_ppi(void) { return platform.cntp_ppi; }
uint32_t dtb_virtio_mmio_count(void) { return platform.virtio_mmio_count; }
const struct aarch64_virtio_mmio_slot *dtb_virtio_mmio_get(uint32_t i)
{
    if (i >= platform.virtio_mmio_count) return NULL;
    return &platform.virtio_mmio[i];
}
enum psci_conduit dtb_psci_conduit(void) { return platform.topology.conduit; }
uint32_t dtb_psci_method(void)
{
    switch (platform.topology.conduit) {
    case PSCI_CONDUIT_SMC: return 0;
    case PSCI_CONDUIT_HVC: return 1;
    default: return UINT32_MAX;
    }
}
uint64_t dtb_mpidr(uint32_t i)
{
    return i < platform.topology.cpu_count ? platform.topology.mpidr[i] : UINT64_MAX;
}
uint32_t mpidr_to_logical_id(uint64_t mpidr)
{
    mpidr &= AARCH64_MPIDR_AFFINITY_MASK;
    for (uint32_t i = 0; i < platform.topology.cpu_count; ++i)
        if (platform.topology.mpidr[i] == mpidr) return i;
    return UINT32_MAX;
}

static __attribute__((noreturn)) void dtb_fatal(const char *reason)
{
    kputs("[dtb] FATAL: ");
    kputs(reason);
    kputs("\n");
    for (;;) __asm__ __volatile__("wfi" ::: "memory");
}

static void acpi_init_platform(const struct boot_context *handoff)
{
    struct aarch64_acpi_info acpi;
    if (aarch64_acpi_parse(handoff->firmware.acpi_rsdp,
                           (uint64_t)AARCH64_BOOT_MAP_END, &acpi) != 0)
        dtb_fatal("ACPI tables invalid or outside UEFI handoff copy");

    struct aarch64_platform_info parsed;
    for (uint32_t i = 0; i < sizeof(parsed); ++i)
        ((uint8_t *)&parsed)[i] = 0;
    parsed.topology.conduit = PSCI_CONDUIT_SMC;
    parsed.topology.psci_compatible = true;
    parsed.topology.cpu_count = acpi.cpu_count <= AARCH64_BOOT_MAX_CPUS
        ? acpi.cpu_count : AARCH64_BOOT_MAX_CPUS;
    for (uint32_t i = 0; i < parsed.topology.cpu_count; ++i)
        parsed.topology.mpidr[i] = acpi.cpu_mpidr[i] & AARCH64_MPIDR_AFFINITY_MASK;
    parsed.gicd_base = acpi.gicd_base;
    parsed.gicc_base = acpi.gicc_base;
    parsed.gicr_base = acpi.gicr_base;
    parsed.gic_version = acpi.gic_version;
    parsed.pl011_base = acpi.spcr_base;
    parsed.cntp_ppi = (acpi.cntp_ppi >= 16u && acpi.cntp_ppi < 32u)
        ? acpi.cntp_ppi : 30u;

    if (parsed.topology.cpu_count == 0 || parsed.gicd_base == 0)
        dtb_fatal("ACPI missing CPUs or GICD");

    uint64_t bsp;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(bsp));
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    for (uint32_t i = 1; i < parsed.topology.cpu_count; ++i) {
        if (parsed.topology.mpidr[i] == bsp) {
            parsed.topology.mpidr[i] = parsed.topology.mpidr[0];
            parsed.topology.mpidr[0] = bsp;
            break;
        }
    }

    for (uint32_t i = 0; i < sizeof(platform); ++i)
        ((volatile uint8_t *)&platform)[i] = ((uint8_t *)&parsed)[i];

    kputs("[acpi] /cpus: ");
    kputu(platform.topology.cpu_count);
    kputs(" CPUs, BSP MPIDR=0x");
    kputx(platform.topology.mpidr[0]);
    kputs("\n");
    kputs("[smp] topology source=uefi-acpi cpus=");
    kputu(platform.topology.cpu_count);
    kputs("\n");
    kputs("[acpi] uart=0x");
    kputx(platform.pl011_base);
    kputs(" gicd=0x");
    kputx(platform.gicd_base);
    kputs(" gicr=0x");
    kputx(platform.gicr_base);
    kputs(" ver=");
    kputu(platform.gic_version);
    kputs("\n");
}

void dtb_init(const struct boot_context *handoff)
{
    /* Validate the dynamically allocated DTB buffer: must be 8-byte
     * aligned, inside the M0 early identity map [0, AARCH64_BOOT_MAP_END),
     * and disjoint from the kernel boot block. */
    const uint64_t map_end = (uint64_t)AARCH64_BOOT_MAP_END;
    const uint64_t b_start = (uint64_t)AARCH64_BOOT_BLOCK_BASE;
    const uint64_t b_end = (uint64_t)AARCH64_BOOT_BLOCK_END;
    if (!boot_context_valid(handoff))
        dtb_fatal("UEFI handoff invalid");
    if (!(handoff->flags & BOOT_CONTEXT_HAS_DTB)) {
        if (handoff->flags & BOOT_CONTEXT_HAS_ACPI) {
            acpi_init_platform(handoff);
            return;
        }
        dtb_fatal("UEFI handoff has no DTB");
    }
    uint64_t addr = handoff->firmware.dtb;
    if (addr == 0 || addr >= map_end || 40 > map_end - addr || (addr & 7))
        dtb_fatal("DTB header outside UEFI handoff copy");
    if (addr < b_end && addr + 40 > b_start)
        dtb_fatal("DTB header outside UEFI handoff copy");
    const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
    uint32_t size = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 |
                    (uint32_t)b[6] << 8 | b[7];
    if (size < 40 || size > map_end - addr)
        dtb_fatal("DTB size outside UEFI handoff copy");
    if (addr < b_end && addr + size > b_start)
        dtb_fatal("DTB size outside UEFI handoff copy");
    uint64_t bsp;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(bsp));
    struct aarch64_platform_info parsed;
    int rc = aarch64_dtb_parse_for_console(
        b, size, bsp, aarch64_boot_early_console_pa(),
        aarch64_boot_console_firmware_configured(), &parsed);
    if (rc == -1) dtb_fatal("malformed UEFI DTB");
    if (rc == -2) dtb_fatal("invalid CPU topology");
    if (rc == -3) dtb_fatal("invalid PSCI binding");
    if (rc) dtb_fatal("unsupported or missing platform devices");
    for (uint32_t i = 0; i < sizeof(platform); ++i)
        ((volatile uint8_t *)&platform)[i] = ((uint8_t *)&parsed)[i];
    if (aarch64_dtb_parse_pcie(b, size, &s_pcie_topology) < 0) {
        for (uint32_t i = 0; i < sizeof(s_pcie_topology); ++i)
            ((volatile uint8_t *)&s_pcie_topology)[i] = 0;
        kputs("[dtb] warn: PCIe DTB parse failed; disabling PCIe\n");
    }
    if (aarch64_dtb_parse_rp1_xhci(b, size, &s_rp1_xhci_desc) < 0) {
        for (uint32_t i = 0; i < sizeof(s_rp1_xhci_desc); ++i)
            ((volatile uint8_t *)&s_rp1_xhci_desc)[i] = 0;
    }
    if (aarch64_dtb_parse_rp1_irq(b, size, platform.gicd_base,
                                  aarch64_boot_early_console_pa(),
                                  &s_rp1_irq_desc) < 0) {
        for (uint32_t i = 0; i < sizeof(s_rp1_irq_desc); ++i)
            ((volatile uint8_t *)&s_rp1_irq_desc)[i] = 0;
    }
    kputs("[dtb] /cpus: ");
    kputu(platform.topology.cpu_count);
    kputs(" CPUs, BSP MPIDR=0x");
    kputx(platform.topology.mpidr[0]);
    kputs("\n");
    kputs("[smp] topology source=uefi-dtb cpus=");
    kputu(platform.topology.cpu_count);
    kputs("\n");
    /* The translated device addresses, for bring-up on boards whose buses
     * remap them: a wrong PL011 here is a console printing into nowhere, a
     * wrong GIC is a silent hang at the first interrupt. */
    kputs("[dtb] pl011=0x");
    kputx(platform.pl011_base);
    kputs(" spi=");
    kputu(platform.pl011_spi);
    kputs(" clk=");
    kputu(platform.pl011_clock);
    kputs(" gicd=0x");
    kputx(platform.gicd_base);
    kputs(" gicc=0x");
    kputx(platform.gicc_base);
    kputs("\n");
}
