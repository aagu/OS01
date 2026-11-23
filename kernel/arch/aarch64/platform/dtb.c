/* UEFI-only DTB handoff wrapper. Pure parsing lives in dtb_parse.c. */
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/cpu.h>
#include <core/bootinfo.h>
#include <stddef.h>

void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

_Static_assert(AARCH64_BOOT_MAX_CPUS == NR_CPUS, "DTB/CPU capacity mismatch");

static struct aarch64_platform_info platform;

const struct aarch64_topology *dtb_topology(void) { return &platform.topology; }
uint32_t dtb_cpu_count(void) { return platform.topology.cpu_count; }
uint64_t dtb_gicd_base(void) { return platform.gicd_base; }
uint64_t dtb_gicc_base(void) { return platform.gicc_base; }
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

void dtb_init(const struct boot_context *handoff)
{
    /* Validate the dynamically allocated DTB buffer: must be 8-byte
     * aligned, inside the M0 early identity map [0, AARCH64_BOOT_MAP_END),
     * and disjoint from the kernel boot block. */
    const uint64_t map_end = (uint64_t)AARCH64_BOOT_MAP_END;
    const uint64_t b_start = (uint64_t)AARCH64_BOOT_BLOCK_BASE;
    const uint64_t b_end = (uint64_t)AARCH64_BOOT_BLOCK_END;
    if (!boot_context_valid(handoff) || !(handoff->flags & BOOT_CONTEXT_HAS_DTB))
        dtb_fatal("UEFI handoff has no DTB");
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
    int rc = aarch64_dtb_parse(b, size, bsp, &parsed);
    if (rc == -1) dtb_fatal("malformed UEFI DTB");
    if (rc == -2) dtb_fatal("invalid CPU topology");
    if (rc == -3) dtb_fatal("invalid PSCI binding");
    if (rc) dtb_fatal("unsupported or missing platform devices");
    for (uint32_t i = 0; i < sizeof(platform); ++i)
        ((volatile uint8_t *)&platform)[i] = ((uint8_t *)&parsed)[i];
    kputs("[dtb] /cpus: ");
    kputu(platform.topology.cpu_count);
    kputs(" CPUs, BSP MPIDR=0x");
    kputx(platform.topology.mpidr[0]);
    kputs("\n");
    kputs("[smp] topology source=uefi-dtb cpus=");
    kputu(platform.topology.cpu_count);
    kputs("\n");
}
