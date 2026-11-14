#ifndef _ARCH_AARCH64_USER_VM_INTERNAL_H
#define _ARCH_AARCH64_USER_VM_INTERNAL_H

/*
 * AArch64 anonymous-VM policy ↔ fork shared internals (private).
 *
 * kernel/arch/aarch64/memory/user_vm_fork.c and user_vm_policy.c are
 * the two physical owners of the resident chain, the VMA metadata and
 * the direct-mapped root accessor.  This header is the only surface
 * between them: no rollback detail and no leaf/backend shape crosses
 * it — the public policy header stays the user-visible contract.
 */

#include <stdint.h>

struct mm_struct;
typedef struct mm_struct mm_t;
typedef struct user_vm_page user_vm_page_t;

/* Managed user root of mm as a raw page-table pointer (the mm's pgdir
 * direct-mapped).  Policy entry points and the fork transaction both
 * reach the M3 primitives through it. */
uint64_t *aarch64_user_vm_root_va(mm_t *mm);

/* Resident-chain maintenance on mm's NULL-terminated, VA-sorted chain
 * (the records themselves are defined in <arch/aarch64/user_vm_policy.h>). */
void aarch64_user_vm_resident_insert(mm_t *mm, user_vm_page_t *node);
void aarch64_user_vm_resident_unlink(mm_t *mm, user_vm_page_t *node);

/* Shared no-file VMA drain: unlink and free every VMA node of mm.
 * Used by fork rollback (child metadata reset) and user_vm_destroy. */
void aarch64_user_vm_free_vmas(mm_t *mm);

#endif /* _ARCH_AARCH64_USER_VM_INTERNAL_H */
