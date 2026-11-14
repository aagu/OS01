#ifndef _ARCH_AARCH64_VMM_BACKEND_H
#define _ARCH_AARCH64_VMM_BACKEND_H

/*
 * aarch64 VMM backend constants (aarch64 M3.2 Task 14).
 *
 * Bit positions / software-bit encodings for the arch_vmm_* aarch64
 * backend (implemented in Task 15/16 on top of page_table.c's
 * aarch64_pt_* primitives).  Descriptor-format bits mirror the
 * AARCH64_PT_* defines in kernel/arch/aarch64/memory/page_table.c.
 */

#include <stdint.h>

// ── Descriptor format bits (AArch64 VMA(format) L3 leaf / L2 block) ──
#define AARCH64_PT_DESC_VALID   UINT64_C(0x001)   /* bit 0 */
#define AARCH64_PT_DESC_TABLE   UINT64_C(0x002)   /* bit 1: table / L3 page */
/* L2 block descriptor: bit 1 == 0. */
#define AARCH64_PT_ATTRINDX_SHIFT 2               /* bits [7:2] MAIR AttrIdx */
#define AARCH64_PT_ATTR_NORMAL  UINT64_C(0x004)   /* AttrIdx 1 (WBWA) */
#define AARCH64_PT_ATTR_DEVICE  UINT64_C(0x000)   /* AttrIdx 0 (nGnRnE) */
#define AARCH64_PT_AP_USER      UINT64_C(0x040)   /* AP[1] bit 6: EL0 access */
#define AARCH64_PT_AP_RO        UINT64_C(0x080)   /* AP[2] bit 7: read-only */
#define AARCH64_PT_DESC_AF      UINT64_C(0x400)   /* bit 10 */
#define AARCH64_PT_DESC_PXN     UINT64_C(0x20000000000000)  /* bit 53 */
#define AARCH64_PT_DESC_UXN     UINT64_C(0x40000000000000)  /* bit 54 */

// ── Software bits (unused descriptor bits, ignored by hardware) ──
// Spec §4.3: software PROT_NONE stash keeps the PA with Valid cleared;
// COW marks fork-shared writable pages.  x86 uses PTE bits 9/10 for
// the same software state (PAGE_PROTNONE / PAGE_COW in pte.h).
#define AARCH64_PT_SOFTWARE_PROTNONE  (1UL << 55)
#define AARCH64_PT_SOFTWARE_COW       (1UL << 56)

// Custom negative sentinel returned by arch_vmm_query_4k /
// arch_vmm_unmap_4k when the entry holds a PROT_NONE stash (distinct
// from -ENOENT / -EINVAL).  This is the ONLY non-Linux-errno value
// that crosses the arch_vmm_* boundary: every other error exit is
// normalized to Linux errno by vmm_backend.c's pt_err_to_linux() —
// the AARCH64_PT_E* enum values (page_table.h) are internal to the
// page-table layer and must not leak through the backend.
#define AARCH64_PT_EPROT_NONE  (-1111)

// ── Arch-neutral semantic page-permission bits (spec §4.2) ──
// Moved here from <memory/vmm.h> (mm-architecture-boundaries Task 2):
// these bits describe THIS backend's semantic query/update API and the
// ARM user VM.  They are not a cross-architecture mapping API.
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
// All functions take the address-space root as a raw pointer (uint64_t *).
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

// Pointer to a page-table level (root or intermediate).  Replaces the old
// `#define mmap uint64_t*` type macro (ARCH-7): that macro poisoned the name
// `mmap` repo-wide, forcing save/restore blocks around every `ops->mmap`
// field.  A typedef collides with nothing.
typedef uint64_t *ptbl_t;

extern ptbl_t kernel_map;

/* M4 checked COW replacement on a managed user root.

 *
 * The only backend entry that may change a user leaf's PA.  Requires a
 * detached, unpublished managed user root (the same write gate as the
 * other 4 KiB user entries), a present RO/NX leaf at `virt` whose PA
 * is exactly old_pa, and a different allocated new_pa.  The new leaf
 * is RW/NX and the replacement goes through the M3
 * aarch64_pt_replace_4k break-before-make path.  Ownership of old_pa
 * stays with the caller — this entry never frees a page.  A kernel_map
 * call, a stash, a permission mismatch or an unallocated/new==old PA
 * is -EINVAL.  Returns 0 on success. */
int aarch64_user_vm_replace_cow(uint64_t *pgdir, uint64_t virt,
                                uint64_t old_pa, uint64_t new_pa);

#endif
