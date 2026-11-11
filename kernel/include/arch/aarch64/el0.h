#ifndef OS01_AARCH64_EL0_H
#define OS01_AARCH64_EL0_H

#include <arch/aarch64/regs.h>
#include <stdbool.h>

/* M4 probe only. The caller owns the leaf
 * pages; a root owns only its table pages. No mappings may change during
 * a run. Use only roots created and populated through this interface. */
uint64_t *aarch64_el0_root_create(void);
int aarch64_el0_map(uint64_t *root, uint64_t va, uint64_t pa, uint32_t perm);
void aarch64_el0_root_destroy(uint64_t *root);

/* Return true to resume the saved EL0 frame, false to end the run.
 * The kernel stack and handler state must be reachable through TTBR1.
 * The handler must not block, schedule, or enable interrupts. Use IRQ
 * mode only on the single BSP after timer startup, before scheduling. */
typedef bool (*aarch64_el0_handler)(pt_regs_t *regs, uint64_t esr,
                                   uint64_t far, void *arg);
int aarch64_el0_run(uint64_t *root, uint64_t pc, uint64_t sp, uint64_t x0,
                   bool irq, aarch64_el0_handler handler, void *arg);

struct aarch64_el0_start {
    uint64_t *root;
    uint64_t pc, sp, x0;
};

/* Runs with IRQs masked on the live EL0 frame after the timer's GIC EOI.
 * Like the sync callback, it must not block, schedule, or enable IRQs. */
typedef void (*aarch64_el0_timer_handler)(pt_regs_t *regs, void *arg);

/* Run starts[0], with starts[1] parked at its entry point. Both roots
 * must be distinct. A NULL timer callback selects the pre-SMP run with
 * IRQs masked; a callback selects the single-BSP timer stage. */
int aarch64_el0_run_pair(const struct aarch64_el0_start starts[2],
                        aarch64_el0_handler handler,
                        aarch64_el0_timer_handler timer, void *arg);

/* Only from the pair's SVC or timer callback, with its live frame.
 * A successful swap arranges for ERET to resume the peer. */
bool aarch64_el0_swap(pt_regs_t *regs);

/* Lower-EL IRQ adapter only, after dispatching a real CNTP tick. */
void aarch64_el0_timer_return(pt_regs_t *regs);

/* Private root while the probe or one of its handlers is executing. */
uint64_t *aarch64_el0_active_root(void);

#endif
