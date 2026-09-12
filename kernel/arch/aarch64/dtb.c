/* UEFI-only DTB handoff wrapper. Pure parsing lives in dtb_parse.c. */
#include <arch/aarch64/dtb.h>
#include <arch/cpu.h>
#include <core/bootinfo.h>

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
uint32_t dtb_cntp_ppi(void) { return platform.cntp_ppi; }
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
    const uint64_t start = UINT64_C(0x401e0000);
    const uint64_t end = UINT64_C(0x401ff000);
    if (!boot_context_valid(handoff) || !(handoff->flags & BOOT_CONTEXT_HAS_DTB))
        dtb_fatal("UEFI handoff has no DTB");
    uint64_t addr = handoff->firmware.dtb;
    /* Validate the header range before the first load, then the full copy.
     * The last handoff page is executable trampoline storage, never DTB. */
    if (addr < start || addr > end || 40 > end - addr || (addr & 7))
        dtb_fatal("DTB header outside UEFI handoff copy");
    const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
    uint32_t size = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 |
                    (uint32_t)b[6] << 8 | b[7];
    if (size < 40 || size > end - addr)
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
