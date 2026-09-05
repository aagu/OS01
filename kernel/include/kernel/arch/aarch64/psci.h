#ifndef OS01_AARCH64_PSCI_H
#define OS01_AARCH64_PSCI_H

#include <stdint.h>
#include <kernel/arch/aarch64/dtb.h>

/* Select and validate the DTB-provided PSCI conduit. */
int psci_init(enum psci_conduit conduit);

/* Start target_mpidr at entry and return the PSCI int32 status code. */
int32_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry,
                    uint64_t context_id);

/* AAPCS64 transport leaves: x0..x3 are the PSCI call registers. */
uint64_t psci_call_smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t psci_call_hvc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3);

#endif
