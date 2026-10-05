#ifndef _ARCH_X86_64_PTE_H
#define _ARCH_X86_64_PTE_H

/*
 * x86_64-private PTE layer (aarch64 M3.2 Task 14).
 *
 * Contents moved verbatim from <memory/vmm.h> (which is now the
 * arch-neutral semantic layer).  ONLY x86-only files include this
 * header — arch-neutral code must use the VM_* bits from
 * <memory/vmm.h> and the arch_vmm_* API instead.
 */

#include <stdint.h>

//page table attribute

//bit 63 Execution Disable:
#define PAGE_NO_EXEC       (1UL << 63)
//bit 12 Page Attribute Table:
#define PAGE_PAT      (1UL << 12)
//bit 8 Global Page:1,global;0,part
#define PAGE_GLOBAL   (1UL << 8)
//bit 7 Page Size:1,big page;0,small page — "this entry is a leaf"
#define PAGE_HUGE     (1UL << 7)
//bit 6 Dirty:1,dirty;0,clean
#define PAGE_DIRTY    (1UL << 6)
//bit 5 Accessed:1,visited;0,unvisited
#define PAGE_ACCESSED (1UL << 5)
//bit 4 Page Level Cache Disable
#define PAGE_CACHE_DISABLE   (1UL << 4)
//bit 3 Page Level Write Through
#define PAGE_WRITE_THROUGH   (1UL << 3)
//bit 2 User Supervisor:1,user and supervisor;0,supervisor
#define PAGE_USER     (1UL << 2)
//bit 1 Read Write:1,read and write;0,read
#define PAGE_WRITE    (1UL << 1)
//bit 0 Present:1,present;0,not present
#define PAGE_VALID    (1UL << 0)

// Conventional aliases (the historical kernel code spells bits 0/1
// PAGE_VALID/PAGE_WRITE; these names are the same hardware bits).
#define PAGE_PRESENT  PAGE_VALID
#define PAGE_RW       PAGE_WRITE

// Hierarchy wrappers. Naming uses the Linux / ARM PGD-PUD-PMD-PTE
// convention (matches aarch64's native nomenclature; the x86_64
// hardware registers are PML4 / PDPT / PDE / PTE -- the two naming
// schemes are equivalent). The bit positions above are still
// x86_64 PTE-format-specific; an aarch64 port defines its own
// constants in arch/aarch64/ headers and re-routes vmm.c through
// the arch_vmm_* backend.
#define PAGE_KERNEL_PGD     (PAGE_WRITE     | PAGE_VALID)
#define PAGE_KERNEL_PUD     (PAGE_WRITE     | PAGE_VALID)
#define PAGE_KERNEL_PMD     (PAGE_HUGE      | PAGE_WRITE     | PAGE_VALID)
// MMIO (uncacheable): PCD=1, PWT=1 for Strong Uncacheable (UC)
#define PAGE_KERNEL_PMD_NOCACHE  (PAGE_HUGE | PAGE_WRITE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH | PAGE_VALID)
#define PAGE_USER_PGD       (PAGE_USER      | PAGE_WRITE     | PAGE_VALID)
#define PAGE_USER_PUD       (PAGE_USER      | PAGE_WRITE     | PAGE_VALID)
#define PAGE_USER_PMD       (PAGE_HUGE      | PAGE_USER      | PAGE_WRITE     | PAGE_VALID)

// 4KB page table entry flags (no PAGE_HUGE — hardware recognizes as 4KB)
#define PAGE_USER_PTE       (PAGE_USER  | PAGE_WRITE     | PAGE_VALID)   // user R/W 4KB
#define PAGE_USER_PTE_RO    (PAGE_USER  | PAGE_VALID)                    // user read-only 4KB
#define PAGE_KERNEL_PTE     (PAGE_WRITE | PAGE_VALID)                    // kernel 4KB
#define PAGE_PROTNONE       (1UL << 9)   // software bit: PROT_NONE stash marker
// bit 9 is x86_64 PTE ignored. mprotect(PROT_NONE) sets this, clears Valid
// but keeps phys.  mprotect(PROT_READ) walks PTEs to restore Valid + clear this.
// do_munmap/vma_free_all check this bit to know phys is valid for free_4k_page.

#define PAGE_COW            (1UL << 10)  // software bit: COW-shared, write triggers fault
// bit 10 is x86_64 PTE ignored.  Fork sets this on writable PTEs, clears PAGE_WRITE.
// COW fault handler checks this bit; if set with V=1,W=1, resolves COW.

// 4KB PTE functions
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);
void vmm_free_user_map(uint64_t *pgdir);

/* x86 4KB PTE backend helpers (aarch64 M3.2 Task 15).
 *
 * These are the x86-internal raw PTE-level primitives the public
 * vmm_map_4k_page / vmm_unmap_4k_page wrappers in vmm.c delegate to.
 * x86_vmm_unmap_4k_page_with_free owns the free/COW ownership logic:
 *   - PAGE_COW: page_cow_put(phys) returns true ONLY on last ref →
 *     free_4k_page(phys) called exactly once; ZERO free if non-last.
 *   - non-COW: free_4k_page(phys) called exactly once.
 * (v1 review item 9: single free per branch.) */
int  x86_vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                         uint64_t virt, uint64_t flags);
int  x86_vmm_unmap_4k_page_with_free(uint64_t *pgdir, uint64_t virt);
int  x86_vmm_query_4k_page(uint64_t *pgdir, uint64_t virt,
                           uint64_t *phys_out, uint64_t *flags_out);

#endif
