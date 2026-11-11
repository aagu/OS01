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

/* Run starts[0], with starts[1] parked at its entry point. Both roots
 * must be distinct. This is BSP-only before SMP, with IRQs masked. */
int aarch64_el0_run_pair(const struct aarch64_el0_start starts[2],
                        aarch64_el0_handler handler, void *arg);

/* Only from the pair's SVC callback, with its live frame. Return this
 * result from the callback to resume the peer or abort the run. */
bool aarch64_el0_swap(pt_regs_t *regs);

/* Private root while the probe is executing (including its SVC handler). */
uint64_t *aarch64_el0_active_root(void);

#endif
