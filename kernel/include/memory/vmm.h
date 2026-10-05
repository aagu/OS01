#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

/*
 * Public, arch-neutral VMM semantic layer (aarch64 M3.2 Task 14).
 *
 * This header exposes ONLY:
 *   - the VM_* semantic bits and their composite values,
 *   - the arch_vmm_* semantic API declarations,
 *   - the shared mmap type / kernel_map and TLB helpers.
 *
 * It must NOT define any x86 PAGE_* hardware PTE bits; those live in
 * the x86-private <arch/x86_64/pte.h>, which only x86-only files
 * include.  Per-arch descriptor bit encodings and software-bit
 * positions live in <arch/<arch>/vmm_backend.h>.
 */

#include <stdint.h>
#include <stddef.h>
#include <arch/mmu.h>

// ── Arch-neutral semantic page-permission bits (spec §4.2) ──
#define VM_PRESENT      (1UL << 0)   // translation is valid
#define VM_WRITE        (1UL << 1)   // writable
#define VM_USER         (1UL << 2)   // user-accessible
#define VM_NO_EXEC      (1UL << 3)   // execute-never
#define VM_HUGE         (1UL << 4)   // block descriptor (2 MiB on x86_64/aarch64 L2)
#define VM_NOCACHE      (1UL << 5)   // device / uncacheable memory type
#define VM_PROTNONE     (1UL << 6)   // software: PROT_NONE stash (phys kept)
#define VM_COW          (1UL << 7)   // software: COW-shared, write faults

// Composite values (spec §4.2 12-legal-combination table)
#define VM_KERNEL_RW    (VM_PRESENT | VM_WRITE)
#define VM_KERNEL_RO    (VM_PRESENT)
#define VM_USER_RW      (VM_PRESENT | VM_USER | VM_WRITE)
#define VM_USER_RO      (VM_PRESENT | VM_USER)
// Device memory: RW, uncacheable, execute-never.  OR in VM_USER and/or
// VM_HUGE for the user/device and block/device combinations.
#define VM_DEVICE       (VM_PRESENT | VM_WRITE | VM_NOCACHE | VM_NO_EXEC)

// ── Arch-neutral semantic API (backends: Task 15 aarch64, Task 16) ──
// All functions take the address-space root as a raw pointer (mmap).
// vm_flags are VM_* combinations; invalid combinations return -EINVAL
// (e.g. VM_NOCACHE without VM_NO_EXEC).  None of these ever free a
// data page — free/COW ownership stays with the caller-side logic.
int arch_vmm_init(void);
int arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                        uint32_t vm_flags);
int arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                       uint32_t vm_flags, uint64_t *old_phys_out,
                       uint32_t *old_vm_out);
int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *old_vm_out);
/* Three-state query: 0 mapped / AARCH64_PT_EPROT_NONE stashed / -ENOENT. */
int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *vm_out);
int arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                    uint32_t vm_flags);
int arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);

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

// 4KB PTE-level map helpers (x86 backend; raw pgdir)
int      vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                         uint64_t virt, uint64_t flags);
void     vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt);

/* ── Boot-initializer checked table allocation (aarch64 M1 plan
 * Task 5) ─────────────────────────────────────────────────────
 * vmm_init now returns int and walks the kernel_map with this
 * checked helper instead of the legacy get_next_level. The runtime
 * map API (vmm_map_page, vmm_pt_walk) is unchanged — it still uses
 * the unchecked calloc-based get_next_level for compatibility.
 *
 * vmm_boot_alloc_table: allocate one 4 KiB PGD/PUD/PMD table page
 * through the kernel's calloc. Returns 0 on success with *out_pa
 * set to the new page's PA; returns -ENOMEM if the allocation
 * fails (no descriptor is constructed — a NULL descriptor would be
 * a PA=0 entry that the hardware reads as "not present" but the
 * code would still try to use as a PUD/PMD pointer).
 *
 * vmm_get_next_level_checked: same semantics as get_next_level but
 * goes through vmm_boot_alloc_table for the missing intermediate
 * table, returning -ENOMEM instead of constructing a NULL entry.
 * On success *out_pa (if non-NULL) is set to the resolved table
 * page PA (whether the slot was filled or already present). */
int  vmm_boot_alloc_table(uint64_t *out_pa);
int  vmm_get_next_level_checked(uint64_t *current_level, size_t entry,
                                uint64_t flags, uint64_t *out_pa);

// ── TLB shootdown (SMP) ─────────────────────────────
// When modifying shared kernel page tables (kernel_map), other CPUs
// may have stale TLB entries.  Call this instead of flush_tlb() to
// notify all online cores via IPI.  Internally falls through to
// local flush_tlb() when num_cpus ≤ 1.
void tlb_shootdown(void);

#endif
