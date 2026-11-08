/* aarch64 local timer backend — public interface of
 * kernel/arch/aarch64/platform/time.c.
 *
 * Two tiers share one register block:
 *
 *   - GLOBAL time (BSP only): jiffies, the poll timeout, the timer
 *     list / TIMER_SIRQ and the blocker scan.  Only the BSP advances
 *     the global clock; an AP never touches it.
 *   - LOCAL tick (per CPU): every admitted CPU owns a private CNTP
 *     interrupt that is used ONLY for per-CPU / per-task runtime
 *     accounting.  It never advances the global clock.
 *
 * The CNTP configuration is FROZEN by the BSP: the counter frequency
 * is read ONCE, validated, and published (release store; fields
 * first, `ready` last).  Every AP acquires that frozen configuration
 * before arming its own counter.  A per-CPU frequency that differs
 * from the BSP's is a hard failure — the local samples of the two
 * CPUs would not be comparable.
 *
 * 100 Hz is a NOMINAL rate: every really delivered CNTP interrupt is
 * one sample and a lost tick is never backfilled from the wall clock.
 *
 * The BSP owns the GIC handler slot for the CNTP PPI: the handler
 * table is global, so the registration happens exactly once, on the
 * BSP.  The per-CPU entry point never registers a handler.
 */

#ifndef OS01_AARCH64_TIMER_H
#define OS01_AARCH64_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/* Nominal local tick frequency [Hz].  The BSP's global tick runs at
 * the same nominal rate. */
#define AARCH64_TIMER_HZ 100U

/* GIC INTID window of a private peripheral interrupt.  0..15 are
 * SGIs and 32.. are SPIs, so a PPI MUST live in 16..31 (the QEMU
 * virt DTB reports 30 for the CNTP physical timer). */
#define AARCH64_TIMER_PPI_FIRST 16U
#define AARCH64_TIMER_PPI_LAST  31U

/* Frozen CNTP configuration, published once by the BSP and acquired
 * read-only by every AP.  `ready` is the publication flag: the other
 * fields are written first and `ready` is release-stored last. */
struct aarch64_timer_config {
    uint64_t freq;     /* CNTP counter frequency [Hz] */
    uint64_t period;   /* ticks per local tick = freq / AARCH64_TIMER_HZ */
    uint32_t ppi;      /* GIC INTID of the CNTP physical timer */
    uint32_t ready;    /* 0 = not published, 1 = published */
};

/* BSP: register the CNTP IRQ handler (the only place that registers
 * it) and publish the frozen configuration.  This phase does NOT arm
 * any counter — the BSP arms its own counter through
 * aarch64_timer_start_local() right after, exactly like an AP does.
 *
 * Returns false on ANY failure — the caller must fail-stop.  A
 * handler-registration conflict (-2, "already registered") is a hard
 * failure: the slot would belong to a foreign handler. */
bool aarch64_timer_publish_config(void);

/* Arm the LOCAL CNTP of the CALLING CPU from the frozen
 * configuration.  Never registers a GIC handler.
 *
 * Returns false — leaving the local counter DISABLED — when the
 * configuration was not published yet, when the local counter
 * frequency differs from the published one, or when the read-back of
 * CNTP_CTL does not show ENABLE=1 / IMASK=0. */
bool aarch64_timer_start_local(void);

/* BSP: verify that the LOCAL counter of the calling CPU is really
 * running (CNTP_CTL_EL0.ENABLE=1 and IMASK=0).  READ-ONLY: it never
 * re-arms the counter, never registers a handler and never touches
 * the global jiffies or the per-second counter.  Used by the BSP's
 * scheduler-tick start, whose counter was already armed during the
 * early boot phase. */
bool aarch64_timer_local_running(void);
bool aarch64_timer_config_ready(void);

#ifdef OS01_SELFTEST
/* OS01_SELFTEST ONLY — mask / restore the CALLING CPU's local CNTP
 * interrupt.
 *
 * The P2 wake acceptance must prove that a remote wake does NOT
 * depend on the AP's periodic tick: the target masks its own CNTP
 * interrupt before it acknowledges the window, and restores it (CTL +
 * rearm + ISB) only after the window's marker/work has completed.
 *
 * aarch64_sched_tick_test_mask_local():
 *   saves CNTP_CTL_EL0, sets IMASK, executes ISB and reads the
 *   register back.  Returns the SAVED control word, or 0 when the
 *   counter is not running or when IMASK did not stick (a running
 *   counter always has ENABLE set, so 0 can never be a valid saved
 *   value).  The caller MUST treat 0 as a hard failure.
 *
 * aarch64_sched_tick_test_restore_local(saved_ctl):
 *   re-arms the counter with the frozen period, writes the saved
 *   control word back (IMASK cleared), executes ISB and reads the
 *   register back.  A saved_ctl of 0 is a no-op. */
uint64_t aarch64_sched_tick_test_mask_local(void);
void aarch64_sched_tick_test_restore_local(uint64_t saved_ctl);
#endif /* OS01_SELFTEST */

#endif /* OS01_AARCH64_TIMER_H */
