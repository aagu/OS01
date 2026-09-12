#ifndef OS01_AARCH64_SMP_BOOT_CORE_H
#define OS01_AARCH64_SMP_BOOT_CORE_H

#include <arch/aarch64/dtb.h>

enum smp_failure { SMP_FAILURE_NONE, SMP_FAILURE_CPU_ON, SMP_FAILURE_TIMEOUT };
struct smp_boot_ops {
    void *ctx;
    uint64_t (*counter)(void *ctx);
    int32_t (*cpu_on)(void *ctx, uint64_t mpidr, uint64_t entry, uint64_t id);
    uint32_t (*online_acquire)(void *ctx, uint32_t id);
    void (*command_release)(void *ctx, uint32_t id, uint32_t command);
    void (*relax)(void *ctx);
};
struct smp_boot_result {
    uint32_t requested, online, online_mask;
    int32_t psci_rc[AARCH64_BOOT_MAX_CPUS];
    enum smp_failure failure[AARCH64_BOOT_MAX_CPUS];
};

/* Published AP slots must start online=0/go=0. Return 0 for every AP
 * acknowledged, 1 for a frozen partial set, -1 for invalid input.
 * Failure publishes idle (2) to every AP; success leaves commands at 0.
 * Only the benchmark may publish run (1). Result is valid only for 0/1. */
int smp_boot_run(const struct aarch64_topology *topology,
                 uint64_t entry_pa, uint64_t counter_hz,
                 const struct smp_boot_ops *ops,
                 struct smp_boot_result *result);

#endif
