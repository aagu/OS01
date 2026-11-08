#ifndef _SELFTEST_BOOT_IRQ_TIME_FIXTURE_H
#define _SELFTEST_BOOT_IRQ_TIME_FIXTURE_H

/* BOOT-2 isolation fixture (QEMU boot-irq-time variants ONLY).
 *
 * The fixture is linked exclusively into boot-irq-time-<case> kernel
 * variants; the normal kernel carries neither the object nor any
 * marker string.  It counts REAL handler invocations and their REAL
 * paired returns (the entry.S test tail calls boot2_test_irq_return()
 * on the two RESTORE_ALL exits with the original pt_regs pointer), and
 * the BSP prints IRQ-off snapshots at the phase boundaries.
 *
 * A return only counts when a token recorded by a handler matches the
 * returning frame (same cpu/phase/frame) — syscall/#PF/other-IRQ
 * returns and cross-phase stale frames never pair, and an unexpected
 * overlapping token is a failure, never silently overwritten.
 */

#include <stdbool.h>

/* observation phases */
#define B2T_PHASE_CLOSED     (-1)
#define B2T_PHASE_RTC        0
#define B2T_PHASE_BOOTSTRAP  1
#define B2T_PHASE_HANDOFF    2

/* timer sources (boot2_test_timer_irq's `source`) */
#define B2T_SRC_PIT   0
#define B2T_SRC_LAPIC 1

/* handler hooks (called from the real handlers under the test macro) */
void boot2_test_rtc_pf(const void *regs);
void boot2_test_timer_irq(unsigned source, const void *regs);

/* the paired-return counter (called from the entry.S test tail) */
void boot2_test_irq_return(const void *regs);

/* phase-boundary hooks (called from the boot path under the test macro) */
void boot2_test_after_rtc(int result);
void boot2_test_after_runtime_open(void);
void boot2_test_after_handoff(bool lapic);

#endif /* _SELFTEST_BOOT_IRQ_TIME_FIXTURE_H */
