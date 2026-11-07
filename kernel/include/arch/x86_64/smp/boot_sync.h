#ifndef _ARCH_X86_64_SMP_BOOT_SYNC_H
#define _ARCH_X86_64_SMP_BOOT_SYNC_H

/* BOOT-1: bounded acquire wait helper for the x86 BSP two-stage AP
 * handshake (spec §6).
 *
 * Contract (hosttest test_x86_smp_boot_sync pins every clause):
 *   - NULL flag or zero budget → false, flag never touched;
 *   - each round does ONE acquire read first: non-zero → true;
 *   - otherwise one pause; at most `budget` rounds total;
 *   - after the final pause there is NO extra read — a publication that
 *     lands after the last pause is NOT observed;
 *   - reads no current-task state, allocates nothing, never masks IRQs.
 *
 * The budgets are finite polling budgets, not milliseconds: do not
 * describe them as wall-clock deadlines in logs.
 */

/* Required waits (online, scheduler_ok) use this budget; the TSC
 * handshake keeps its own smaller SYNC_SPIN_LIMIT in smp.c. */
#define X86_SMP_READY_SPIN_LIMIT 5000000u

#include <stdint.h>
#include <stdbool.h>

/* Poll `*flag` with acquire loads until it reads non-zero or the budget
 * is exhausted.  Returns true only if a round's read observed non-zero. */
bool x86_smp_wait_u32_acquire(const uint32_t *flag, uint32_t budget);

#endif /* _ARCH_X86_64_SMP_BOOT_SYNC_H */
