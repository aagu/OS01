#ifndef KERNEL_ARCH_SCHED_H
#define KERNEL_ARCH_SCHED_H

/* Arch-neutral scheduling-policy facade (aarch64 AP cooperative
 * scheduler P1, spec §4).
 *
 * Common scheduler code includes ONLY this header — never a per-arch
 * path — so the AP cooperation policy stays arch-neutral.  Each
 * architecture provides the three strong backends below in
 * kernel/arch/<arch>/cpu/sched_notify.c.
 */

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>

/* Default affinity mask for an UNPINNED kernel thread.
 *
 *   x86_64:  every CPU up to num_cpus (the historical reach; unchanged).
 *   aarch64: the constant 1 — ordinary kernel threads stay on CPU0;
 *            explicit kernel_thread_on_cpu() targets carry their own
 *            single-bit mask (spec §4).
 */
uint64_t arch_kthread_default_mask(void);

/* Remote reschedule notification backend.  Called by
 * sched_notify_remote_cpu() AFTER need_resched was published with a
 * release store, and only for a validated remote target.
 *
 *   x86_64:  ipi_send(arch_processor_id, IPI_VECTOR_RESCHED).
 *   aarch64: P1 polling no-op — AP idles poll their runqueue and
 *            need_resched every iteration (the "polling contract");
 *            P2 replaces this with the resched SGI send.
 */
void arch_sched_notify_remote(unsigned cpu);

/* Whether THIS cpu may scan the global blocker list (and therefore
 * whether tasks running here may install blockers at all).
 *
 *   x86_64:  always true.
 *   aarch64: cpu_id() == 0 — only the BSP scans; an AP blocker could
 *            never be discovered by the single scanner, so APs must
 *            not sleep on blocker_wait() (-EOPNOTSUPP).  P2 removes
 *            the AP restriction with the SGI wakeup protocol.
 */
bool arch_sched_scan_blockers(void);
#endif

#endif /* KERNEL_ARCH_SCHED_H */
