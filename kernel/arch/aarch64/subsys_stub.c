/* kernel/arch/aarch64/subsys_stub.c — SUBSYS framework minimal stubs.
 *
 * SUBSYS_INITCALL Task 2 / R3-1 fix calls subsys_init_phase() from
 * kernel/arch/aarch64/main.c, mirroring x86_64 kernel/core/main.c:194.
 * Task 3 flips clocksource.c's SUBSYS_INITCALL gate from
 * #ifdef __x86_64__ to unconditional — at that point
 * _clocksource_register calls register_subsys() to queue its wrapper
 * (_clocksource_init_wrapper) into subsys_table[].
 *
 * Group 3b (Task 4) removes the explicit Option B clocksource_init()
 * call from kernel/arch/aarch64/main.c, so the dispatch path below is
 * the SINGLE source of clocksource_init() invocation. To make that
 * work, this stub provides a real (minimal) subsys_table[] queue —
 * register_subsys actually stores the wrapper, and subsys_init_phase
 * walks the table and invokes each init() matching the requested
 * phase.
 *
 * We deliberately do NOT link kernel/subsys/subsys.c on aarch64 today:
 * that file references serial_printk, num_cpus, strcmp, and the
 * scheduler's idle_resume, none of which the strict aarch64 kernel
 * whitelist (kernel/Makefile:42-44) currently provides. When the port
 * grows enough to link the real subsys.c, drop this stub and add
 * subsys/subsys.c to KERNEL_C_SOURCES.
 *
 * API parity with kernel/subsys/subsys.c is maintained for the BSP
 * register+dispatch path (register_subsys + subsys_init_phase). The
 * per-cpu registration, subsys_init_all, subsys_init_percpu, and
 * subsys_status paths stay as no-ops because no aarch64 driver
 * currently uses them (verified by `grep -R 'SUBSYS_INITCALL' kernel/`
 * for the aarch64 whitelist — only kernel/time/clocksource.c with
 * phase 4 is registered today).
 *
 * The init() call is bare: no serial_printk, no logging — the caller
 * (subsys_init_phase called from kernel/arch/aarch64/main.c between
 * smp_boot_aps and arch_tick_start) relies on clocksource_init()'s
 * own kputs("[clocksource] active=true\n") markers to confirm the
 * dispatch ran. This satisfies qemutests/aarch64_uefi_smp.py
 * --expect-clk.
 */

#include <subsys/subsys.h>

#define MAX_SUBSYS        16   /* enough for aarch64 whitelist today */
#define MAX_SUBSYS_PERCPU  4   /* no aarch64 percpu consumers today */

static subsys_entry_t        subsys_table[MAX_SUBSYS];
static int                   subsys_count = 0;
static subsys_percpu_entry_t subsys_percpu_table[MAX_SUBSYS_PERCPU];
static int                   subsys_percpu_count = 0;

int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    if (!name || !init || subsys_count >= MAX_SUBSYS)
        return -1;
    subsys_entry_t *e = &subsys_table[subsys_count];
    e->name        = name;
    e->init        = init;
    e->phase       = phase;
    e->flags       = flags;
    e->initialized = 0;
    subsys_count++;
    return 0;
}

int register_subsys_percpu(const char *name,
                           int (*init_percpu)(int cpu_id),
                           uint32_t flags)
{
    (void)name; (void)init_percpu; (void)flags;
    /* no aarch64 percpu consumers today; table is reserved for the
     * day kernel/subsys/subsys.c replaces this stub. */
    return 0;
}

void subsys_init_phase(int phase)
{
    for (int i = 0; i < subsys_count; i++) {
        subsys_entry_t *e = &subsys_table[i];
        if (e->phase != phase || e->initialized != 0)
            continue;
        int ret = e->init();
        e->initialized = (ret == 0) ? 1 : -1;
        /* mark FAIL rows as non-fatal — kernel log already contains
         * the explicit "[clocksource]" markers if init succeeded;
         * if it failed, the marker block in main.c prints nothing
         * and --expect-clk will surface the regression naturally. */
        if (ret != 0 && !(e->flags & SUBSYS_FLAG_OPTIONAL))
            ; /* swallow (no serial_printk available on this profile) */
    }
}

void subsys_init_all(void)
{
    for (int phase = SUBSYS_PHASE_3; phase <= SUBSYS_PHASE_6; phase++)
        subsys_init_phase(phase);
}

void subsys_init_percpu(void)
{
    /* intentional no-op: no aarch64 percpu consumers today */
}

int subsys_status(const char *name)
{
    (void)name;
    return 0;
}