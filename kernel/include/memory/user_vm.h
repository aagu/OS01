#ifndef _KERNEL_MEMORY_USER_VM_H
#define _KERNEL_MEMORY_USER_VM_H

/* AArch64 M4 anonymous-VM leaf adapter.
 *
 * The arch-neutral anonymous policy (kernel/memory/user_vm.c, later
 * increments) reasons about a user leaf in three semantic states.
 * The M3 backend exposes them as three different return shapes:
 *
 *   0                       → PRESENT (valid descriptor, PA valid)
 *   -ENOENT                 → ABSENT  (no leaf at the VA)
 *   AARCH64_PT_EPROT_NONE   → STASHED (PA preserved, VALID clear)
 *
 * The adapter below is the only place that translates those shapes;
 * the policy never reads ESR, a descriptor or an M3 sentinel.  The
 * caller owns the root and must have detached it from TTBR0 before
 * calling a mutating entry (same contract as the M3 user-root branch).
 */

#include <stdint.h>

/* Tri-state of one user virtual-address leaf. */
enum user_vm_leaf_state {
    USER_VM_ABSENT = 0,     /* no descriptor at the VA */
    USER_VM_PRESENT,        /* valid, user-accessible descriptor */
    USER_VM_STASHED,        /* PROT_NONE stash: PA preserved, VALID clear */
};

struct user_vm_leaf {
    enum user_vm_leaf_state state;
    uint64_t                pa;      /* valid for PRESENT and STASHED */
    uint32_t                flags;   /* VM_* semantic word (STASHED: VM_PROTNONE) */
};

/* Policy invariant failure: log the reason and stop the kernel.
 * Reached only when a commit-time check contradicts an earlier
 * preflight (the policy never repairs a broken invariant). */
_Noreturn void arch_user_vm_invariant(const char *reason);

/* Query one leaf.  Always returns 0 on success and fills *out with
 * one of the three states; returns the backend error unchanged for
 * any other failure (-EINVAL for a bad VA/window, -EBUSY if the root
 * is busy on a read, etc.). */
int arch_user_vm_query(uint64_t *root, uint64_t va, struct user_vm_leaf *out);

/* Remove one leaf.  Returns 0 when a PRESENT or STASHED leaf was
 * removed and fills *out with the state it had plus the PA that was
 * removed (the caller always owns and must free that PA exactly
 * once).  An ABSENT leaf returns 0 with *out->state == USER_VM_ABSENT
 * and pa == 0.  Other failures pass through unchanged. */
int arch_user_vm_remove(uint64_t *root, uint64_t va, struct user_vm_leaf *out);

#endif /* _KERNEL_MEMORY_USER_VM_H */
