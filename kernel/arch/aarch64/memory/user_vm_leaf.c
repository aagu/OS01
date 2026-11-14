/* kernel/arch/aarch64/memory/user_vm_leaf.c
 *
 * AArch64 leaf adapter for the arch-neutral anonymous-VM policy.
 *
 * This unit is the single translation point between the M3 user-root
 * backend's return shapes and the policy's three semantic states; the
 * policy never sees -ENOENT or the AARCH64_PT_EPROT_NONE sentinel.
 * It performs no root switching, no IRQ control and no allocation —
 * the caller owns the mm, has detached the root from TTBR0, and is
 * the one that frees a PA returned by a successful remove.
 *
 * The transaction layer (authorization, IRQ/TLS registration, root
 * detach/restore, syscall/fault navigation) lives in user_vm.c.
 */

#include <arch/aarch64/page_table.h>
#include <arch/aarch64/user_vm_leaf.h>    /* this TU's API + leaf states */
#include <arch/aarch64/vmm_backend.h>     /* arch_vmm_* / AARCH64_PT_*  */
#include <arch/cpu.h>
#include <errno.h>
#include <log/log.h>
#include <memory/vmm.h>                   /* VM_PROTNONE */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Normalize one backend result into *out.  Only the three documented
 * outcomes are translated; everything else is returned unchanged and
 * *out is left zeroed/ABSENT so a caller that ignores the error can
 * never observe a stale leaf. */
static int normalize_leaf(int rc, uint64_t pa, uint32_t vm,
                          struct user_vm_leaf *out)
{
    if (rc == 0) {
        out->state = USER_VM_PRESENT;
        out->pa    = pa;
        out->flags = vm;
        return 0;
    }
    if (rc == -ENOENT) {
        out->state = USER_VM_ABSENT;
        out->pa    = 0;
        out->flags = 0;
        return 0;
    }
    if (rc == AARCH64_PT_EPROT_NONE) {
        out->state = USER_VM_STASHED;
        out->pa    = pa;
        out->flags = VM_PROTNONE;
        return 0;
    }
    out->state = USER_VM_ABSENT;
    out->pa    = 0;
    out->flags = 0;
    return rc;
}

int arch_user_vm_query(uint64_t *root, uint64_t va, struct user_vm_leaf *out)
{
    if (!out)
        return -EINVAL;

    uint64_t pa = 0;
    uint32_t vm = 0;
    int rc = arch_vmm_query_4k(root, va, &pa, &vm);
    return normalize_leaf(rc, pa, vm, out);
}

int arch_user_vm_remove(uint64_t *root, uint64_t va, struct user_vm_leaf *out)
{
    if (!out)
        return -EINVAL;

    uint64_t pa = 0;
    uint32_t vm = 0;
    int rc = arch_vmm_unmap_4k(root, va, &pa, &vm);
    /* The stashed case keeps the PA in `pa` (the backend's unmap
     * contract), so normalize_leaf reports it as removed-and-owned. */
    return normalize_leaf(rc, pa, vm, out);
}

/* Checked COW replacement: the policy names this adapter entry; the
 * M3 backend owns the root gate, the old-leaf check and the actual
 * break-before-make replacement. */
int arch_user_vm_replace_cow(uint64_t *root, uint64_t va, uint64_t old_pa,
                             uint64_t new_pa)
{
    return aarch64_user_vm_replace_cow(root, va, old_pa, new_pa);
}

_Noreturn void arch_user_vm_invariant(const char *reason)
{
    log_err("[user-vm] invariant violated: %s\n",
            reason ? reason : "(no reason)");
    for (;;)
        arch_cpu_halt();
}
