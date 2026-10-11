// kernel/arch/x86_64/subsys_percpu.c

#include <subsys/subsys.h>
#include <intr/apic.h>

static int _lapic_timer_start_percpu(int cpu_id)
{
    /* Monitor plan Task 2: deliberately SKIP both the hardware start
     * and the accounting publish here.
     *
     * This callback runs ON THE BSP, once per cpu index, after the APs
     * are already online:
     *   - a lapic_timer_start() call here programs the EXECUTING core's
     *     own LAPIC, so it would just redundantly restart the BSP's
     *     timer num_cpus times (duplicate hardware start);
     *   - the BSP's accounting lifecycle is published exactly once by
     *     arch_tick_start() (LAPIC success or enabled-PIT fallback);
     *   - each AP's accounting lifecycle is published exactly once by
     *     ap_entry(), from the REAL lapic_timer_start() return value.
     *     An AP whose timer failed to start must stay valid=0
     *     (unavailable) — re-publishing started=true from here would
     *     override that genuine failure state with a bogus "running".
     */
    (void)cpu_id;
    return 0;
}

void arch_register_subsys_percpu(void)
{
    register_subsys_percpu("lapic-timer-start", _lapic_timer_start_percpu, 0);
}
