#ifndef KERNEL_SELFTEST_AARCH64_SCHED_TICK_H
#define KERNEL_SELFTEST_AARCH64_SCHED_TICK_H

/* kernel/include/selftest/aarch64_sched_tick.h — the per-CPU tick
 * evidence producer (per-CPU tick accounting, Task 4).
 *
 * The product selftest publishes one evidence row per case and per
 * CPU.  The rows are collected ON the CPU they describe: the counter
 * increments happen in the target CPU's own context, the snapshot is
 * taken with the target's interrupts masked, and the result is
 * release-published into a persistent slot that the BSP reads with an
 * acquire load.  Only the BSP prints the complete evidence rows.
 *
 * The observer hooks below are called from the PRODUCTION paths (the
 * local tick, the BSP's global tick, the idle WFI boundary, the GIC
 * SGI dispatch and the arch switch).  They exist only in the
 * dedicated tick selftest build (AARCH64_SCHED_TICK_TEST); every
 * other build never references them.
 */

#include <stdint.h>
#include <stdbool.h>

#if defined(OS01_SELFTEST) && defined(AARCH64_SCHED_TICK_TEST)

/* The local tick observer.  global=false is a successful scheduling
 * sample (one per delivered local tick); global=true is the BSP's
 * completed global tick.  The counters are per-CPU: there is no
 * shared increment anywhere. */
void aarch64_sched_tick_observe(unsigned cpu, bool global);

/* The idle WFI observer: the target records its sleep boundary, so a
 * remote wake can be distinguished from a periodic-timer wake. */
void aarch64_sched_tick_wfi_enter(unsigned cpu);
void aarch64_sched_tick_wfi_return(unsigned cpu);

/* The GIC SGI trace: every SGI4 entry and its matching EOI. */
void aarch64_sched_tick_sgi_trace(unsigned cpu, uint32_t iar, bool after_eoi);

/* The shared scheduler observers (test_aarch64_scheduler.c) report a
 * switch and a nested schedule() that happened while an interrupt
 * was active. */
void aarch64_sched_tick_irq_switch(unsigned cpu);
void aarch64_sched_tick_irq_nested(unsigned cpu);

/* The self-switch tripwire: every arch switch that asked to switch
 * to the CURRENT task is counted.  It must stay zero. */
void aarch64_sched_tick_self_switch(unsigned cpu);

/* The idle-window extension command handler.  The ap_work extension
 * dispatcher (the kernel keeps exactly ONE strong ap_work_ext_run())
 * forwards WORK_SCHED_TICK_IDLE here: the TARGET CPU then measures
 * its OWN idle window inside its own idle context.  The handler runs
 * on the target with interrupts enabled and returns true when the
 * command was consumed. */
bool aarch64_sched_tick_idle_work(uint64_t *out);

/* The remote-SGI extension command handler: the AP sends a REAL
 * resched SGI4 and a TLB SGI3 to the BSP, so the BSP can be the
 * TARGET of a remote interrupt exchange.  Returns true when the
 * command was consumed. */
bool aarch64_sched_tick_send_sgi_work(uint64_t *out);

/* The sleep probe (kernel/arch/aarch64/cpu/idle.c): ONE interruptible
 * sleep on the CALLING CPU that records the boundary through the SAME
 * observers the idle step uses.  The remote case needs a really
 * sleeping target: the BSP's own idle step never reaches its wait (the
 * BSP tick requests a reschedule on every tick), so the probe supplies
 * the missing sleep explicitly. */
void arch_aarch64_tick_sleep_probe(void);

/* The assembly probe (kernel/arch/aarch64/cpu/probe.S):
 *
 *   int aarch64_sched_tick_irq_probe(const uint64_t *local_count,
 *                                     uint64_t target,
 *                                     uint64_t deadline_cycles);
 *
 * It waits for a REAL interrupt of the CALLING CPU: the local tick
 * counter the caller passes in must reach `target` (the caller reads
 * the counter of ITS OWN CPU, never the BSP's), and it must STRICTLY
 * grow beyond the snapshot the probe takes at its own entry — a tick
 * that landed before the probe started never interrupted it.  The
 * caller-saved register sentinels must survive the interrupt entry.
 * The deadline is a cntvct budget.
 *
 * Returns 0 = ok, 1 = register corruption, 2 = SP drift, 4 = timeout. */
int aarch64_sched_tick_irq_probe(const uint64_t *local_count, uint64_t target,
                                  uint64_t deadline_cycles);

/* The evidence driver: runs the seven cases on every admitted CPU
 * and publishes the whole evidence batch BEFORE the matching
 * selftest_record_late() records. */
void test_aarch64_sched_tick_run(void);

#endif /* OS01_SELFTEST && AARCH64_SCHED_TICK_TEST */

#endif /* KERNEL_SELFTEST_AARCH64_SCHED_TICK_H */
