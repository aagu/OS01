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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <list.h>

struct mm_struct;
typedef struct mm_struct mm_t;

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

/* ── Sparse resident-page record ────────────────────────────────
 *
 * One node per committed anonymous page, in mm->resident_list,
 * sorted by va.  The chain head is zero-initialised (calloc'd mm_t),
 * so the links are NULL-terminated rather than circular: a head with
 * `next == NULL` is empty.  Nodes are always the policy's own
 * kmalloc'd records; ports never share them with another owner.
 *
 * A resident record is the OWNER of its PA: every destroy/unmap path
 * frees the page exactly once, after verifying the PA the adapter
 * handed back. */
typedef struct user_vm_page {
    list_t   list;
    uint64_t va;
    uint64_t pa;
} user_vm_page_t;

/* ── Anonymous-VM policy ──────────────────────────────────────
 *
 * Every mutating entry takes an mm whose user root is DETACHED from
 * TTBR0 and unpublished; the caller is the single owner for the
 * duration (single-core BSP, IRQ-off commit discipline in the arch
 * wrapper).  The policy itself performs no root switching, no IRQ
 * masking and no scheduling.
 *
 * Address windows (AArch64 first version):
 *   heap  [heap_base, 0x203ff000)       brk-owned, VMA_HEAP
 *   mmap  [0x40000000, AARCH64_USER_LIMIT)
 * ELF and stack pages stay owned by the image manifest; they are
 * never recorded in resident_list. */

/* Install the zero-length heap VMA and start/end_brk.  heap_base is
 * rounded up to 4 KiB.  Fails with -ENOMEM if the VMA cannot be
 * allocated and leaves the mm unchanged. */
int user_vm_init_heap(mm_t *mm, uint64_t heap_base);

/* Release every anonymous resident page, then every policy-owned
 * VMA.  The root must already be detached; the caller then hands the
 * empty mm to aarch64_user_mm_destroy(). */
void user_vm_destroy(mm_t *mm);

/* True iff the mm still carries VMA metadata or resident records.
 * The user-root destroy path refuses such an mm even when the page
 * table already has no leaves. */
bool user_vm_has_metadata(const mm_t *mm);

/* brk: 0 queries, otherwise grow/shrink the heap.  Grow commits
 * metadata only; shrink releases fully-outside pages and zeroes the
 * retained tail through the PA direct-map alias.  Returns the new
 * break or -EINVAL/-ENOMEM. */
int64_t user_vm_brk(mm_t *mm, uint64_t addr);

/* mmap(addr,len,prot,flags,fd,off) — anonymous MAP_PRIVATE only.
 * Reserves a VMA; no physical page is allocated.  Returns the chosen
 * VA or a negative errno. */
int64_t user_vm_mmap(mm_t *mm, const uint64_t args[6]);

/* mprotect over mmap VMAs only.  Updates resident leaves of the
 * covered pages and the VMA description of the rest. */
int64_t user_vm_mprotect(mm_t *mm, uint64_t addr, uint64_t len,
                         uint64_t prot);

/* munmap over the mmap window; holes are allowed and are a no-op.
 * Removes overlapping VMA fragments and resident pages. */
int64_t user_vm_munmap(mm_t *mm, uint64_t addr, uint64_t len);

/* Demand-fill one EL0 data-abort address.  Returns 0 when the caller
 * must retry the same PC, -EFAULT when the address is not authorised
 * (guard/hole/permission/leaf already present), -ENOMEM on
 * allocation failure. */
int user_vm_fault(mm_t *mm, uint64_t far, bool write);

/* Pre-instantiate every absent page of [addr, addr+len) that is
 * authorised by a heap or anonymous VMA.  Validation is a complete
 * read-only pass first: any hole, PROT_NONE page or insufficient
 * permission returns -EFAULT before a single page is created.  New
 * leaves are staged; a failure rolls back exactly the leaves created
 * by this call and returns -ENOMEM. */
int user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write);

#endif /* _KERNEL_MEMORY_USER_VM_H */
