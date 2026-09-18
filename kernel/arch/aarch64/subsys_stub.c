/* kernel/arch/aarch64/subsys_stub.c — SUBSYS framework no-op stubs.
 *
 * SUBSYS_INITCALL Task 2 / R3-1 fix calls subsys_init_phase() from
 * kernel/arch/aarch64/main.c, mirroring x86_64 kernel/core/main.c:194.
 * Task 3 flips clocksource.c's SUBSYS_INITCALL gate from
 * #ifdef __x86_64__ to unconditional — at that point
 * _clocksource_register calls register_subsys() to queue its wrapper.
 *
 * We deliberately do NOT link kernel/subsys/subsys.c on aarch64 today:
 * that file references serial_printk, num_cpus, strcmp, and the
 * scheduler's idle_resume, none of which the strict aarch64 kernel
 * whitelist (kernel/Makefile:42-44) currently provides. When the port
 * grows enough to link the real subsys.c, drop this stub and add
 * subsys/subsys.c to KERNEL_C_SOURCES.
 *
 * The stubs here provide the symbols _clocksource_register calls
 * (register_subsys, register_subsys_percpu, subsys_init_phase,
 * subsys_init_all, subsys_init_percpu, subsys_status) with empty
 * table semantics. register_subsys returns 0 (success) without
 * queueing; subsys_init_phase walks an empty table (no-op).
 * Functionally identical to a kernel/subsys/subsys.c build with no
 * drivers registered.
 */

#include <subsys/subsys.h>

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    (void)name; (void)init; (void)phase; (void)flags;
    return 0;
}

int register_subsys_percpu(const char *name,
                           int (*init_percpu)(int cpu_id),
                           uint32_t flags)
{
    (void)name; (void)init_percpu; (void)flags;
    return 0;
}

void subsys_init_phase(int phase)
{
    (void)phase;
    /* intentional no-op: empty __subsys_init range on aarch64 */
}

void subsys_init_all(void)
{
    /* intentional no-op: empty table */
}

void subsys_init_percpu(void)
{
    /* intentional no-op: empty table */
}

int subsys_status(const char *name)
{
    (void)name;
    return 0;
}
