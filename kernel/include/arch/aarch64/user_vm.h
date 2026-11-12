#ifndef OS01_AARCH64_USER_VM_H
#define OS01_AARCH64_USER_VM_H

/* AArch64 M4 user-VM transaction layer.
 *
 * The anonymous-VM policy in kernel/memory/user_vm.c is arch-neutral
 * and assumes an mm whose root is already detached.  This layer owns
 * the three things that make that true:
 *
 *   1. authorization — a native SVC that will RETURN, or one short
 *      EL0 demand-fault, may enter an edit transaction;
 *   2. detachment — save IRQ + both EL0 TLS registers, mask IRQ,
 *      switch to the kernel root (revoking the user root), mark the
 *      transaction non-nestable, then run the policy with IRQs on;
 *   3. restoration — mask IRQ, install the original user root,
 *      restore TLS, clear the detached state and restore the entry
 *      IRQ state on every return path.
 *
 * Hardware reads/writes live in cpu/user_task.c (frame admission,
 * TTBR0, published-root, root switch and TLS).  The state machine and
 * the wrapper itself are production code; the host suite replaces
 * only those boundary functions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arch/regs.h>
#include <arch/aarch64/sync_fault.h>

struct mm_struct;
typedef struct mm_struct mm_t;
struct intr_event;

/* Translation fault, level 0..3 (FSC 0x04..0x07).  The generic
 * AARCH64_FSC_ACCESS_* window also spans access-flag and permission
 * faults, which are NOT demand-fillable. */
#define AARCH64_FSC_TRANSLATION_FIRST 0x04ULL
#define AARCH64_FSC_TRANSLATION_LAST  0x07ULL

/* ── Pure demand-fault classification ───────────────────────────
 *
 * Eligible: an EL0 Data Abort with a valid FAR, a translation FSC
 * (0x04..0x07), and neither S1PTW, CM nor FnV set.  Access-flag
 * (0x08..0x0b) and permission (0x0c..0x0f) faults are NOT demand
 * faults here — they keep the original termination path.  WnR
 * selects write over read.
 *
 * Inputs only: no runtime state, no system register. */
static inline bool aarch64_user_vm_fault_classify(uint64_t esr,
                                                  bool far_valid,
                                                  bool *write_out)
{
    if (!far_valid)
        return false;
    if (esr & (AARCH64_ESR_ISS_S1PTW | AARCH64_ESR_ISS_CM |
               AARCH64_ESR_FNV_BIT))
        return false;
    if (((esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT) !=
        AARCH64_EC_DATA_ABORT_LOWER)
        return false;

    uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;
    if (fsc < AARCH64_FSC_TRANSLATION_FIRST ||
        fsc > AARCH64_FSC_TRANSLATION_LAST)
        return false;

    if (write_out)
        *write_out = (esr & AARCH64_ESR_ISS_WNR) != 0;
    return true;
}

/* ── Native SVC authorization ───────────────────────────────────
 *
 * enter() registers a transaction authorization only when the frame
 * is the running formal user frame, the current mm is the task's mm
 * and the hardware TTBR0 holds that mm's root (published).  It
 * deliberately skips SYS_exit (non-returning) and any number without
 * a handler.  leave() clears the authorization after the dispatch. */
void aarch64_vm_native_enter(pt_regs_t *regs, uint64_t nr);
void aarch64_vm_native_leave(void);

/* ── BSP IRQ depth ──────────────────────────────────────────────
 * The EL1 IRQ adapter brackets its GIC path with these.  A transaction
 * requires depth zero, so an IRQ handler can never borrow the SVC
 * authorization even on the same task stack. */
void aarch64_vm_irq_enter(void);
void aarch64_vm_irq_leave(void);

/* ── Edit transaction ───────────────────────────────────────────
 * begin() verifies authorization, the current task's mm, the actual
 * TTBR0, the published root, depth zero and the non-nested state; it
 * then detaches the root and enables IRQs.  end() restores
 * everything.  Exposed (rather than static) so the host suite can
 * drive the state machine directly. */
int  aarch64_vm_edit_begin(mm_t *mm);
void aarch64_vm_edit_end(void);

/* ── Policy entry points under the transaction ─────────────────── */

/* SVC adapter used only by the AArch64 subset sys_mm.c branch. */
int64_t arch_syscall_user_vm(uint64_t nr, const uint64_t args[6]);

/* Demand-fill one classified fault.  Returns 0 when the caller must
 * retry the same PC, -EFAULT when the fault is not eligible, and
 * -ENOMEM on policy exhaustion. */
int aarch64_user_vm_fault(pt_regs_t *regs, const struct intr_event *event);

/* Pre-instantiate an authorized copy range, then reinstall the root. */
int aarch64_user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len,
                                 bool write);

/* ── Hardware boundary (cpu/user_task.c; host runtime in tests) ── */
uint64_t aarch64_user_vm_read_ttbr0(void);
bool     aarch64_user_vm_root_published(uint64_t root_pa);
void     aarch64_user_vm_save_tls(uint64_t *tpidr_el0,
                                  uint64_t *tpidrro_el0);
void     aarch64_user_vm_restore_tls(uint64_t tpidr_el0,
                                     uint64_t tpidrro_el0);

#endif /* OS01_AARCH64_USER_VM_H */
