#ifndef KERNEL_ARCH_AARCH64_AP_SCHED_H
#define KERNEL_ARCH_AARCH64_AP_SCHED_H

/* aarch64 AP cooperative-scheduler handoff (P1 spec §3).
 *
 * One-way sequence: the BSP constructs the permanent AP idle tasks on
 * its boot stack (prepare), finishes its own boot probes and switches
 * to its task stack, publishes its own scheduler_ok, then opens the
 * release gate and waits until every accepted AP published scheduler_ok
 * (release).  Each AP's old work loop polls the gate; an accepted AP
 * that sees it open leaves the work stack through the validating
 * assembly trampoline (stack_enter) and lands in the C handoff
 * (idle_handoff) on its permanent idle task stack, which installs
 * rq->idle, enables IRQs and finally publishes scheduler_ok.
 *
 * A late or never-ACKed AP never passes released(): the gate is only
 * open for CPUs in the frozen boot ACK mask, so such an AP stays in
 * its old work loop forever — the previous production behaviour.
 */

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>

struct task_struct;
typedef struct thread_struct thread_t;

/* BSP boot stack, once, after all boot probes and BEFORE the BSP task
 * stack switch.  Constructs the permanent AP idle task unions and
 * thread contexts for every CPU in the frozen accepted mask.  Must not
 * use `current` (the boot stack has none). */
void aarch64_ap_sched_prepare(void);

/* BSP, on its task stack, after its own scheduler_ok publication.
 * Opens the release gate (release store), then waits for every
 * accepted AP's scheduler_ok with a 2-second hardware-counter
 * deadline, IRQs enabled and no lock held.  A missing AP is a
 * fail-stop: the static stacks may already be in use and are never
 * reclaimed. */
void aarch64_ap_sched_release(void);

/* AP-side gate poll: true only when the gate is open AND the frozen
 * accepted mask contains @cpu. */
bool aarch64_ap_sched_released(uint32_t cpu);

/* AP, from its old work loop: mask IRQs and hand off through the
 * validating assembly trampoline.  Never returns. */
void aarch64_ap_sched_enter(uint32_t cpu) __attribute__((noreturn));

/* C handoff, running on the AP's new idle task stack (the trampoline
 * tail-branches here): verifies current/ctx/metadata, installs
 * rq->idle, enables IRQs + ISB and finally publishes scheduler_ok
 * with a release store, then enters the polling idle loop. */
void aarch64_ap_idle_handoff(uint32_t cpu) __attribute__((noreturn));

/* Assembly trampoline (thread_entry.S): validates the spec stack
 * boundaries of (@sp, @ctx) and installs SP before tail-branching to
 * aarch64_ap_idle_handoff(@cpu).  The old AP work stack is simply
 * abandoned. */
void aarch64_ap_stack_enter(uint64_t sp, thread_t *ctx, uint32_t cpu)
    __attribute__((noreturn));
#endif

#endif /* KERNEL_ARCH_AARCH64_AP_SCHED_H */
