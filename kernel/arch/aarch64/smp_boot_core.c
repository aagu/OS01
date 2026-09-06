#include <kernel/arch/aarch64/smp_boot_core.h>

int smp_boot_run(const struct aarch64_topology *topology,
                 uint64_t entry_pa, uint64_t counter_hz,
                 const struct smp_boot_ops *ops,
                 struct smp_boot_result *result)
{
    if (!topology || !ops || !result || !topology->cpu_count ||
        topology->cpu_count > AARCH64_BOOT_MAX_CPUS || !entry_pa ||
        !counter_hz || counter_hz > UINT64_MAX / 2 || !ops->counter ||
        !ops->cpu_on || !ops->online_acquire || !ops->command_release ||
        !ops->relax)
        return -1;

    /* Reject stale ACKs before starting any CPU. go=0 is the caller's
     * publication precondition; this core never publishes go=1. */
    for (uint32_t id = 1; id < topology->cpu_count; ++id)
        if (ops->online_acquire(ops->ctx, id))
            return -1;

    result->requested = topology->cpu_count;
    result->online = 1;
    result->online_mask = 1;
    for (uint32_t id = 0; id < AARCH64_BOOT_MAX_CPUS; ++id) {
        result->psci_rc[id] = 0;
        result->failure[id] = SMP_FAILURE_NONE;
    }
    int degraded = 0;
    const uint64_t timeout = counter_hz * 2;
    for (uint32_t id = 1; id < topology->cpu_count; ++id) {
        const uint64_t start = ops->counter(ops->ctx);
        int32_t rc = ops->cpu_on(ops->ctx, topology->mpidr[id], entry_pa, id);
        result->psci_rc[id] = rc;
        if (rc != 0 && rc != -4 && rc != -5) {
            result->failure[id] = SMP_FAILURE_CPU_ON;
            degraded = 1;
            continue;
        }
        for (;;) {
            /* Check time before the acquire: even an ACK at the exact
             * boundary is excluded. Unsigned elapsed time handles wrap. */
            if ((uint64_t)(ops->counter(ops->ctx) - start) >= timeout) {
                result->failure[id] = SMP_FAILURE_TIMEOUT;
                degraded = 1;
                break;
            }
            if (ops->online_acquire(ops->ctx, id)) {
                ++result->online;
                result->online_mask |= UINT32_C(1) << id;
                break;
            }
            ops->relax(ops->ctx);
        }
    }
    if (degraded) {
        /* Include late, failed and not-yet-running APs. Never reset their
         * slots/stacks or re-read ACKs into the now frozen online set. */
        for (uint32_t id = 1; id < topology->cpu_count; ++id)
            ops->command_release(ops->ctx, id, 2);
    }
    return degraded;
}
