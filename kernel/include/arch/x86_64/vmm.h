#ifndef _ARCH_X86_64_VMM_H
#define _ARCH_X86_64_VMM_H

/*
 * x86_64-private VMM surface (mm-architecture-boundaries Task 2).
 *
 * The x86 page-table implementation (kernel/arch/x86_64/memory/vmm.c)
 * and everything that manipulates raw x86 roots include this header.
 * The arch-neutral public <memory/vmm.h> carries ONLY the shared TLB
 * broadcast; nothing here may leak into it.
 */

#include <stdint.h>
#include <stddef.h>
#include <arch/mmu.h>              /* ARCH_PAGE_OFFSET, arch_flush_tlb_all */
#include <arch/x86_64/mmu.h>       /* arch_get_page_table / arch_switch_mm
                                    * / walkers (arch_virt_to_phys,
                                    * arch_user_range_accessible) */

// Top-level page-table index shift (39 on x86_64).
#define PAGE_PGD_SHIFT 39

// Pointer to a page-table level (root or intermediate).  Replaces the old
// `#define mmap uint64_t*` type macro (ARCH-7): that macro poisoned the name
// `mmap` repo-wide, forcing save/restore blocks around every `ops->mmap`
// field.  A typedef collides with nothing.
typedef uint64_t *ptbl_t;

extern ptbl_t kernel_map;

// Local TLB reload alias: the x86 callers' shorthand for the full CR3
// reload (unmap/TLB ownership plan: the user root is single-task owned
// and active only on the local CPU).
#define flush_tlb()    arch_flush_tlb_all()

// ── Raw page-table API (implementation: arch/x86_64/memory/vmm.c) ──

void x86_vmm_map_2m(uint64_t *pgdir, uintptr_t physical_address,
                    uintptr_t virtual_address, uint64_t flags);
ptbl_t vmm_alloc_map(void);
void vmm_free_user_map(ptbl_t pgdir);

/* vmm_init adopts the boot page table as the kernel map and direct-maps
 * every represented RAM frame.  0 on success; -ENOMEM if any
 * intermediate-table allocation fails — the caller is expected to
 * fatally halt on any non-zero return (kernel/arch/x86_64/memory/boot.c). */
int vmm_init(void);

// Walk PGD->PUD->PMD->PTE, return pointer to PTE[level3] entry.
// If allocate=true, allocates missing intermediate tables via calloc.
// flags carries PAGE_USER for user-accessible intermediate levels.
// Returns NULL if allocate fails (OOM) or a required table is missing
// with allocate=false.
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);

/* ── Boot-initializer checked table allocation (aarch64 M1 plan
 * Task 5) ─────────────────────────────────────────────────────
 * vmm_get_next_level_checked: same semantics as the internal
 * get_next_level but allocates the missing intermediate table
 * (one 4 KiB PGD/PUD/PMD page through the kernel's calloc) with a
 * checked result, returning -ENOMEM instead of constructing a NULL
 * entry.  On success *out_pa (if non-NULL) is set to the resolved
 * table page PA (whether the slot was filled or already present). */
int  vmm_get_next_level_checked(uint64_t *current_level, size_t entry,
                                uint64_t flags, uint64_t *out_pa);

/* ── x86 4 KiB PTE backend helpers (aarch64 M3.2 Task 15).  These own
 * the PAGE_* hardware bit layout and the free/COW ownership
 * semantics.  Leaf release ownership lives in
 * x86_vmm_release_owned_4k: withdraw with x86_vmm_take_4k, sync,
 * then release — never free inline. */
int  x86_vmm_map_4k(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags);
int  x86_vmm_query_4k_page(uint64_t *pgdir, uint64_t virt,
                           uint64_t *phys_out, uint64_t *flags_out);

/* ── Allocation-free leaf withdraw / release (munmap transaction) ──
 *
 * x86_vmm_take_4k saves the full old descriptor (PRESENT or PROTNONE)
 * and clears the slot.  It NEVER frees the data page or changes a COW
 * count — the caller must synchronize the TLB first and then call
 * x86_vmm_release_owned_4k for leaves it owns.
 *
 * x86_vmm_release_owned_4k performs the ownership transfer for one
 * old descriptor: non-COW pages are freed once; COW pages are put
 * once and freed only by the last holder.  Borrowed device leaves
 * must never reach this function.
 *
 * x86_vmm_has_2m reports a user 2 MiB block leaf covering va;
 * x86_vmm_take_2m (2 MiB-aligned va only) clears and returns it.
 * The 2 MiB pages themselves stay with their current owner. */
uint64_t x86_vmm_take_4k(uint64_t *root, uint64_t va);
void     x86_vmm_release_owned_4k(uint64_t old_pte);
bool     x86_vmm_has_2m(uint64_t *root, uint64_t va);
uint64_t x86_vmm_take_2m(uint64_t *root, uint64_t va);

/* ── Intermediate-table preparation for a range (device MAP_FIXED) ──
 *
 * Allocate every missing PGD/PUD/PMD/PT table covering the 4 KiB
 * aligned [start, end) range WITHOUT writing a leaf.  Every published
 * table gets an undo record allocated first; on any allocation
 * failure the parent slots are restored in reverse publish order, the
 * local sync runs, and then the new tables are freed.  Huge-leaf
 * conflicts and addresses outside the user half return -EINVAL before
 * any allocation.  After success every 4 KiB slot is writable without
 * further allocation.
 *
 * x86_vmm_local_sync is the local CR3 reload used by that rollback.
 * It is weak in the kernel so host harnesses can observe the
 * clear -> sync -> free order without executing privileged asm. */
int  x86_vmm_prepare_4k_range(uint64_t *root, uint64_t start,
                              uint64_t end);
void x86_vmm_local_sync(void);

#endif /* _ARCH_X86_64_VMM_H */
