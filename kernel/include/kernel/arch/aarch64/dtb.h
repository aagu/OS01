#ifndef OS01_AARCH64_DTB_H
#define OS01_AARCH64_DTB_H

#include <stdint.h>
#include <stdbool.h>

#define AARCH64_BOOT_MAX_CPUS 8
#define AARCH64_MPIDR_AFFINITY_MASK UINT64_C(0x000000ff00ffffff)
enum psci_conduit { PSCI_CONDUIT_NONE, PSCI_CONDUIT_SMC, PSCI_CONDUIT_HVC };
struct aarch64_topology {
    uint64_t mpidr[AARCH64_BOOT_MAX_CPUS];
    uint32_t cpu_count;
    enum psci_conduit conduit;
    bool psci_compatible;
};
struct aarch64_platform_info {
    struct aarch64_topology topology;
    uint64_t gicd_base, gicc_base, pl011_base;
    uint32_t cntp_ppi;
};

/* 0 success; -1 malformed blob; -2 topology; -3 PSCI; -4 platform.
 * Every failure clears out when non-NULL. No hardware or global state. */
int aarch64_dtb_parse(const void *blob, uint32_t size, uint64_t bsp,
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
uint32_t dtb_cntp_ppi(void);
enum psci_conduit dtb_psci_conduit(void);
/* Legacy accessor encoding: SMC=0, HVC=1, unavailable=UINT32_MAX. */
uint32_t dtb_psci_method(void);

#endif
