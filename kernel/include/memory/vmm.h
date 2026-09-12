#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <arch/mmu.h>

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

// Hierarchy wrappers. Naming uses the Linux / ARM PGD-PUD-PMD-PTE
// convention (matches aarch64's native nomenclature; the x86_64
// hardware registers are PML4 / PDPT / PDE / PTE -- the two naming
// schemes are equivalent). The bit positions above are still
// x86_64 PTE-format-specific; an aarch64 port will need to redefine
// these constants in an arch/<arch>/page.h header and re-route
// vmm.c through that.
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
int      vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                         uint64_t virt, uint64_t flags);
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate);

// Validate-and-lock a user write range before the kernel writes into it
// (getrandom / devfs random read).  Returns 0 with current->mm->pgdir_lock HELD
// on success — caller MUST call user_write_range_end().  Returns -EFAULT
// (lock NOT held) on any bad address or non-writable page.  Kernel buffers
// (current->mm == NULL) are trusted and pass through without lock/check.
int  user_write_range_begin(uint64_t addr, size_t len);
void user_write_range_end(void);

#define KERNEL_MEM_OFFSET 0xffffffff80000000
#define PHYS_MEM_OFFSET 0xffff800000000000

#define mmap uint64_t*

extern mmap kernel_map;

// Backward-compatible aliases for existing callers
#define flush_tlb()    arch_flush_tlb_all()
#define switch_tlb(p)  arch_switch_mm((uint64_t *)(p))

void vmm_map_page(uint64_t *pgdir, uintptr_t physical_address,
                  uintptr_t virtual_address, uint64_t flags);
uintptr_t vmm_unmap_page(uint64_t *pgdir, uintptr_t virtual_address);
mmap vmm_alloc_map(void);
void vmm_free_user_map(mmap pgdir);

// ── TLB shootdown (SMP) ─────────────────────────────
// When modifying shared kernel page tables (kernel_map), other CPUs
// may have stale TLB entries.  Call this instead of flush_tlb() to
// notify all online cores via IPI.  Internally falls through to
// local flush_tlb() when num_cpus ≤ 1.
void tlb_shootdown(void);

#endif
