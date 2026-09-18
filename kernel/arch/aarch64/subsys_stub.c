/* kernel/arch/aarch64/subsys_stub.c — subsys_init_phase() no-op stub.
 *
 * SUBSYS_INITCALL Task 2 / R3-1 fix calls subsys_init_phase() from
 * kernel/arch/aarch64/main.c, mirroring x86_64 kernel/core/main.c:194.
 * Until Task 3 flips clocksource.c's gate from #ifdef __x86_64__ to
 * unconditional, no driver on aarch64 actually emits a SUBSYS_INITCALL
 * entry, so the dispatch walk is over an empty subsys_table and is a
 * no-op by definition.
 *
 * We deliberately do NOT link kernel/subsys/subsys.c on aarch64 today:
 * that file references serial_printk, num_cpus, strcmp, and the
 * scheduler's idle_resume, none of which the strict aarch64 kernel
 * whitelist (kernel/Makefile:42-44) currently provides. When the port
 * grows enough to link the real subsys.c, drop this stub and add
 * subsys/subsys.c to KERNEL_C_SOURCES.
 *
 * For Task 2 verification: this stub exists SOLELY so the
 * register+dispatch pair compiles and links on aarch64 without
 * pulling in scheduler / percpu / libc-string infrastructure that
 * the port has not yet built. Functionally it is identical to
 * subsys_init_phase() walking an empty subsys_table.
 */

#include <subsys/subsys.h>

void subsys_init_phase(int phase)
{
    (void)phase;
    /* intentional no-op: empty __subsys_init range on aarch64 */
}
