#ifndef _ARCH_AARCH64_USER_VM_POLICY_H
#define _ARCH_AARCH64_USER_VM_POLICY_H

/* AArch64 anonymous-VM policy (mm-architecture-boundaries Task 3).
 *
 * Moved here verbatim from <memory/user_vm.h> together with its owner
 * kernel/arch/aarch64/memory/user_vm_policy.c (formerly
 * kernel/memory/user_vm.c).  The policy hard-codes this port's address
 * window and relies on the managed user root; the transaction entry
 * points remain in kernel/arch/aarch64/memory/user_vm.c and the leaf
 * adapter in kernel/arch/aarch64/memory/user_vm_leaf.c (declared in
 * <arch/aarch64/user_vm_leaf.h>).  The two VM policies are deliberately NOT
 * merged and mm_t keeps its layout.
 *
 * Unified ownership of ELF / stack / anonymous residency: the sparse
 * resident records below are the single owner of every user page's
 * PA, source classification, logical permission and share state. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <list.h>

struct mm_struct;
typedef struct mm_struct mm_t;

/* ── Sparse resident-page record ────────────────────────────────
 *
 * One node per committed user page, in mm->resident_list, sorted by
 * va.  The chain head is zero-initialised (calloc'd mm_t), so the
 * links are NULL-terminated rather than circular: a head with
 * `next == NULL` is empty.  Nodes are always the policy's own
 * kmalloc'd records; ports never share them with another owner.
 *
 * A resident record is the OWNER of its PA: every destroy/unmap path
 * removes and verifies its leaf, then releases the PA exactly once.
 * The record also carries the three facts a hardware leaf cannot
 * express:
 *
 *   - source  — image (ELF/stack) or anonymous (heap/mmap);
 *   - logical — the permission the user asked for, even while a
 *               shared leaf is hardware RO/NX;
 *   - share   — exclusive ownership, or a counted reference held in
 *               the PMM COW map.
 *
 * The ELF and stack leaves enter the same chain through
 * user_vm_map_image_page(); there is no separate image manifest. */

enum user_vm_page_source {
    USER_VM_PAGE_ANON = 0,  /* heap / private anonymous mmap */
    USER_VM_PAGE_ELF,       /* image ELF segment leaf */
    USER_VM_PAGE_STACK,     /* image user stack leaf */
};

enum user_vm_page_share {
    USER_VM_PAGE_PRIVATE = 0, /* owns its PA; PMM COW count is zero */
    USER_VM_PAGE_SHARED,      /* holds one counted reference */
};

typedef struct user_vm_page {
    list_t   list;
    uint64_t va;
    uint64_t pa;
    uint32_t source;   /* enum user_vm_page_source */
    uint32_t logical;  /* logical VM_* permission */
    uint32_t share;    /* enum user_vm_page_share */
} user_vm_page_t;

/* Take ownership of one already-populated image page and map it at va
 * with the exact VM_* permission `flags` (RO/X, RO/NX or RW/NX).
 *
 * The record is allocated first, then the leaf mapped; on failure
 * nothing is recorded and `pa` is still the caller's to free.  On
 * success the mm owns both the record and the PA: user_vm_destroy()
 * removes the leaf, verifies the returned PA and frees it exactly
 * once.  Callers must have a detached root and an initialised heap
 * VMA (the same contract as user_vm_init_heap). */
int user_vm_map_image_page(mm_t *mm, uint64_t va, uint64_t pa,
                           uint32_t flags);

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
 * ELF and stack leaves are recorded too (through
 * user_vm_map_image_page) so the mm owns every user page, but the
 * heap/mmap policy entries keep operating only on their own windows. */

/* Install the zero-length heap VMA and start/end_brk.  heap_base is
 * rounded up to 4 KiB.  Fails with -ENOMEM if the VMA cannot be
 * allocated and leaves the mm unchanged. */
int user_vm_init_heap(mm_t *mm, uint64_t heap_base);

/* ── fork cloning (M4 Task 2) ────────────────────────────────────
 *
 * Build a child address space from a parent whose root is already
 * detached, then publish the child task as the LAST fallible step.
 *
 * The parent's brk/layout and every VMA node are copied, and every
 * resident page is shared: each child leaf borrows the parent's PA
 * (no reference yet), then the commit acquires one checked PMM COW
 * reference per page (private 0→2, already-shared +1), lowers a
 * writable parent leaf to RO/NX, and finally calls publish(arg).
 *
 * Any failure before publish leaves the parent's leaves, records and
 * counts exactly as they were, removes only the child leaves and
 * metadata, and returns the error; the caller destroys the (now
 * empty) child mm to reclaim intermediate tables.  A publish failure
 * rolls the whole commit back the same way.  A commit-time mismatch
 * between a record, its leaf and the PMM count is an invariant
 * failure (the kernel stops).
 *
 * The child mm must be fresh: unpublished, with an empty resident
 * chain and no VMA nodes.  It keeps its own root. */
int user_vm_clone_fork(mm_t *parent, mm_t *child,
                       int (*publish)(void *), void *arg);

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

/* CoW write fault on a shared resident page (EL0 FSC 0x0f, WnR=1).
 *
 * Requires a shared record with logical write permission and a
 * present RO/NX leaf at that PA.  A page with more than one reference
 * is copied into a fresh page, the leaf is replaced through the
 * checked COW entry and the old reference dropped; a page holding the
 * only reference is upgraded in place to RW/NX and its exclusive
 * reference released without freeing the PA.  Returns 0 (retry the
 * PC), -EFAULT for a state that is not a legal COW target, or -ENOMEM
 * when the private copy cannot be allocated.  Nothing changes on
 * failure. */
int user_vm_cow_fault(mm_t *mm, uint64_t far);

/* Pre-instantiate every absent page of [addr, addr+len) that is
 * authorised by a heap or anonymous VMA.  Validation is a complete
 * read-only pass first: any hole, PROT_NONE page or insufficient
 * permission returns -EFAULT before a single page is created.  New
 * leaves are staged; a failure rolls back exactly the leaves created
 * by this call and returns -ENOMEM. */
int user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write);

#endif /* _KERNEL_MEMORY_USER_VM_H */
