#ifndef OS01_AARCH64_DTB_H
#define OS01_AARCH64_DTB_H

#include <stdint.h>
#include <stdbool.h>
#include <config/limits.h>

#define AARCH64_BOOT_MAX_CPUS NR_CPUS
#define AARCH64_MPIDR_AFFINITY_MASK UINT64_C(0x000000ff00ffffff)
enum psci_conduit { PSCI_CONDUIT_NONE, PSCI_CONDUIT_SMC, PSCI_CONDUIT_HVC };
struct aarch64_topology {
    uint64_t mpidr[AARCH64_BOOT_MAX_CPUS];
    uint32_t cpu_count;
    enum psci_conduit conduit;
    bool psci_compatible;
};
#define AARCH64_VIRTIO_MMIO_MAX 32   // QEMU virt exposes 32 virtio-mmio slots
struct aarch64_virtio_mmio_slot {
    uint64_t base;         /* device register window base (512-byte window) */
    uint32_t spi;          /* GIC SPI INTID (32 + DTB nr), 0 = unknown/poll */
};
struct aarch64_platform_info {
    struct aarch64_topology topology;
    uint64_t gicd_base, gicc_base, pl011_base;
    uint32_t cntp_ppi;
    uint32_t pl011_spi;    /* PL011 RX GIC SPI INTID (QEMU virt: 33) */
    uint32_t pl011_clock;  /* PL011 uartclk in Hz (0 if unspecified in DTB) */
    /* Optional virtio-mmio transport slots (QEMU virt: 0x0a000000 + i*0x200,
     * SPI 48 + i).  Collected, not validated against fixed constants: the
     * consumer probes MAGIC/VERSION/DEVICEID at runtime. */
    struct aarch64_virtio_mmio_slot virtio_mmio[AARCH64_VIRTIO_MMIO_MAX];
    uint32_t virtio_mmio_count;
};

/* 0 success; -1 malformed blob; -2 topology; -3 PSCI; -4 platform.
 * Every failure clears out when non-NULL. No hardware or global state. */
int aarch64_dtb_parse(const void *blob, uint32_t size, uint64_t bsp,
                      struct aarch64_platform_info *out);
int aarch64_dtb_parse_for_console(const void *blob, uint32_t size,
                                  uint64_t bsp, uint64_t console_pa,
                                  bool firmware_configured,
                                  struct aarch64_platform_info *out);
struct boot_context;
void dtb_init(const struct boot_context *handoff);
const struct aarch64_topology *dtb_topology(void);
uint32_t dtb_cpu_count(void);
uint64_t dtb_mpidr(uint32_t i);
uint32_t mpidr_to_logical_id(uint64_t mpidr);
uint64_t dtb_gicd_base(void);
uint64_t dtb_gicc_base(void);
uint64_t dtb_pl011_base(void);
uint32_t dtb_pl011_spi(void);
uint32_t dtb_pl011_clock(void);
uint32_t dtb_cntp_ppi(void);
/* virtio-mmio transport slots discovered in the DTB (optional nodes). */
uint32_t dtb_virtio_mmio_count(void);
const struct aarch64_virtio_mmio_slot *dtb_virtio_mmio_get(uint32_t i);
enum psci_conduit dtb_psci_conduit(void);
/* Legacy accessor encoding: SMC=0, HVC=1, unavailable=UINT32_MAX. */
uint32_t dtb_psci_method(void);

#endif
