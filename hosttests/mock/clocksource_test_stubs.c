/* hosttests/mock/clocksource_test_stubs.c — link-time symbols for the
 * host clocksource test. The production kernel/time/clocksource.c and the
 * inline clocksource_read_ns() in <time/clocksource.h> reference:
 *
 *   - jiffies                    (extern, from <time/timer.h>)
 *   - percpu_data[0]             (extern, our host percpu_t stub)
 *   - arch_cycle_freq()          (extern, was static inline aarch64 asm)
 *   - arch_cycle_counter()       (extern, was static inline aarch64 asm)
 *   - register_subsys()          (only under #ifdef __x86_64__ — host build
 *                                 defines __x86_64__, so the production
 *                                 SUBSYS_INITCALL block emits a call)
 *
 * arch_cycle_freq / arch_cycle_counter are intentionally NOT static
 * inline here — the production header was short-circuited (see
 * clocksource_test_runtime.h), so they resolve to these extern symbols
 * at link time. The test TU writes host_mock_cycle_freq /
 * host_mock_cycle_counter to drive behaviour.
 */
#include "clocksource_test_runtime.h"
#include <subsys/subsys.h>

volatile uint64_t jiffies      = 0;
uint64_t host_mock_cycle_freq  = 62500000ULL;   /* QEMU virt CNTP default */
uint64_t host_mock_cycle_counter = 0;

percpu_t percpu_data[1];

uint64_t arch_cycle_freq(void)    { return host_mock_cycle_freq; }
uint64_t arch_cycle_counter(void) { return host_mock_cycle_counter; }

/* The host build defines __x86_64__ but we don't pull in real subsys
 * machinery (it's collected via .subsys_init linker section at kernel
 * link time). Provide a no-op so the production SUBSYS_INITCALL block
 * resolves. */
int register_subsys(const char *name, int (*init)(void),
                    int phase, uint32_t flags)
{
    (void)name; (void)init; (void)phase; (void)flags;
    return 0;
}

