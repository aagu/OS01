# COW (Copy-on-Write) Fork — Design Specification

> Date: 2026-07-10
> Status: approved
> Scope: kernel-only, ~250 lines, 7 files

## Motivation

`fork_mm_copy` currently does EAGER copying: every writable 2MB/4KB user page
is fully duplicated via `alloc_pages`/`alloc_4k_page` + `memcpy`. In the common
`fork+exec` pattern (shell spawning a command), the child process immediately
discards these copies — the work is wasted entirely.  Fork latency scales with
process size (up to ~100ms for a 2MB data segment) instead of being bounded by
page-table walk time (~1μs).

COW (Copy-on-Write) fixes this: fork shares pages read-only between parent
and child; the first writer triggers a page fault that allocates only the
4KB page actually being written.  This makes fork near-instant and eliminates
wasted copies on exec.

## Design: Page-Level COW via cow_count

**Approach A**: per-physical-page reference count (`cow_count`), tagged in the
PTE with `PAGE_COW` (bit 10).  Fork clears `PAGE_R_W` on every writable PTE,
sets `PAGE_COW`, and increments the backing physical page's `cow_count`.
The fault handler checks `cow_count`:
- `> 1` → allocate new page + copy + decrement old ref
- `== 1` → restore `PAGE_R_W` in-place (zero-copy — last-sharer path)

Tracking is unified: `struct Page` handles both 2MB and 4KB COW counts.
`phys_to_page(phys)` works for every physical address in the system
(`PMMngr.pages_struct` is indexed by 2MB frame number), so there is no
separate pool-level bookkeeping.

- **2MB huge pages**: `page->cow_count` tracks the shared count.
  On first COW fault the page is split to 4KB (see §4); after split,
  `cow_sub[]` takes over per-slot tracking.
- **4KB pages** (from `alloc_4k_page` or post-split): `page->cow_sub[slot]`
  tracks the shared count. `cow_sub` is lazily allocated on first use
  (1 KB malloc per 2MB frame that has COW-shared 4KB pages).

### Why not mm-level refcounting (Approach B)?

Parent and child have independent PML4 + PTEs. When the child COW-faults a
page, only the child's PTE is updated to point at the new physical page; the
parent's PTE still points at the old one. Without per-physical-page tracking,
the kernel cannot determine when the old physical page is unreferenced,
leading to memory leaks or dangling PTEs.

### Why not blind-free-on-COW-fault (Approach C)?

Without a refcount, the COW fault handler must always either leak the old page
(if another process still shares it) or free it (risking a dangling PTE in
the other process). Neither is correct.

## Components

### 1. Data Structures

#### 1.1 New PTE software bit (`vmm.h`)

```c
#define PAGE_COW  (1UL << 10)  // bit 10: COW-shared, write triggers fault
```

Bit 10 in x86-64 PTEs is ignored by hardware (available to software).  
Bit 9 is already used by `PAGE_PROTNONE`.

#### 1.2 `struct Page` extension (`pmm.h`)

```c
struct Page {
    // ... existing fields unchanged ...
    uint16_t  cow_count;   // 2MB-granularity COW reference count
    uint16_t *cow_sub;     // NULL until split: 512-element 4KB sub-counts
};
```

#### 1.3 Helper functions (`pmm.c` + `pmm.h`)

```c
// Map any physical address to its struct Page*.
// PMMngr.pages_struct is indexed by 2MB frame number:
//   idx = phys >> PAGE_2M_SHIFT
//   return &PMMngr.pages_struct[idx]
static inline struct Page *phys_to_page(uint64_t phys);

// Increment/decrement/query the COW refcount for a physical address.
// Automatically routes to the correct granularity:
//   - cow_sub != NULL → cow_sub[slot]  (4KB or split-2MB)
//   - cow_sub == NULL → cow_count       (unsplit 2MB huge page)
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);   // returns true when count reaches zero
uint16_t page_cow_refs(uint64_t phys);
```

### 2. `fork_mm_copy`: Share, Don't Copy

Path | Condition | Action
--- | --- | ---
A | PTE is writable (`PAGE_R_W` set) | Clear `PAGE_R_W` + set `PAGE_COW` on **both** parent and child PTEs; `page_cow_get(phys)` |
B | PTE is read-only | Share PTE directly — no COW tracking (`.text` / `.rodata` / `VM_READ`-only pages) |

Key detail for Path A: the parent's in-memory PTE is modified in-place
(`parent_pml2[l2] &= ~PAGE_R_W; parent_pml2[l2] |= PAGE_COW;`). The child's
new page-table page gets a copy of this same (R/O + COW) PTE.

At the end of `fork_mm_copy`: `flush_tlb()` broadcast via `tlb_shootdown()`.
Without this, the parent's TLB still caches the old writable mapping and
writes will silently succeed without triggering the COW fault.

The existing eager-copy loop is replaced wholesale. Fallback on OOM:
if `page_cow_get` somehow fails (impossible in current code — it's just
a counter increment), fall through to the readonly share path (Path B).

### 3. `do_page_fault`: COW Fault Handling

Inserted **after** the VMA permission check and **before** the existing
not-present (demand-paging) path:

```c
// P=1, W=1  (write to a present page → possible COW)
if ((error_code & 0x03) == 0x03) {
    uint64_t *pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
    if (!pte || !(*pte & PAGE_COW))
        goto sigsegv;  // write to read-only non-COW page

    uint64_t old_phys = *pte & PAGE_4K_MASK;

    // 2MB huge page?  Split before handling
    if (*pte & PAGE_PS) {
        split_2mb_to_4k(user_pml4, cr2, old_phys, vma->vm_page_prot);
        pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
        // fall through to 4KB path below
    }

    // Now guaranteed 4KB PTE
    if (page_cow_refs(old_phys) > 1) {
        uint64_t new_phys = alloc_4k_page();
        if (!new_phys) { kill_current_user_task(regs); return; }
        memcpy(Phy_To_Virt(new_phys), Phy_To_Virt(old_phys), PAGE_4K_SIZE);
        *pte = new_phys | vma->vm_page_prot;
        page_cow_put(old_phys);
    } else {
        // Last reference — restore writable, zero copy
        *pte = old_phys | vma->vm_page_prot;
    }
    flush_tlb();
    return;
}
```

The existing P=0 demand-paging path (VM_ANON / VM_FILE) is unchanged.

### 4. `split_2mb_to_4k`: 2MB Huge Page → 4KB PTEs

When a COW-fault hits a 2MB huge-page PDE, we cannot copy 2MB — that defeats
COW. Instead, split the 2MB entry into 512 4KB PTEs, all pointing at their
respective sub-page with R/O + COW.

```c
void split_2mb_to_4k(uint64_t *pml4, uint64_t fault_va,
                     uint64_t phys_2m, uint64_t vma_prot) {
    struct Page *pg = phys_to_page(phys_2m);

    // Allocate cow_sub if not already done
    if (!pg->cow_sub) {
        pg->cow_sub = kmalloc(512 * sizeof(uint16_t));
        // Broadcast current cow_count to all 512 slots
        for (int i = 0; i < 512; i++)
            pg->cow_sub[i] = pg->cow_count;
        pg->cow_count = 0;  // 2MB count delegated to sub-array
    }

    // Walk to the PDE
    uint64_t *pml2 = /* ... walk pml4→pml3→pml2 ... */;
    uint64_t *pde = &pml2[(fault_va >> 21) & 0x1FF];

    // Allocate a 4KB PTE table
    uint64_t *pte_table = calloc(1, PAGE_4K_SIZE);

    // Fill 512 PTEs: each points to its 4KB sub-page, R/O + COW
    for (int i = 0; i < 512; i++) {
        uint64_t sub_phys = phys_2m + i * PAGE_4K_SIZE;
        pte_table[i] = sub_phys | PAGE_U_S | PAGE_Present | PAGE_COW;
        // Note: R/W=0 (read-only to trigger COW)
    }

    // Replace the PDE: was 2MB huge page, now a 4KB PTE table pointer
    *pde = Virt_To_Phy((uint64_t)pte_table) | PAGE_USER_Dir;

    flush_tlb();
}
```

After this, the fault VA's PTE is a normal 4KB entry. The COW fault handler
falls through to the 4KB path. The sub-count for the faulting slot
(`pg->cow_sub[slot]`) is checked: if >1, allocate + copy + decrement; if ==1,
restore writable.

**Memory overhead**: 1024 bytes per split 2MB page.  Only pages that are
actually written post-fork trigger a split; read-only pages (`.text`) never
split. Typical overhead: ~0 KB for a shell that only execs.

### 5. Exit / Exec / Munmap: Decrement cow_count

#### 5.1 `vmm_unmap_4k_page` (`vmm.c`)

```c
void vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt) {
    uint64_t *pte = vmm_pt_walk(pagemap, virt, 0, 0);
    if (!pte) return;
    if (!(*pte & (PAGE_Present | PAGE_PROTNONE))) return;

    uint64_t phys = *pte & PAGE_4K_MASK;

    if (*pte & PAGE_COW) {
        if (page_cow_put(phys))  // true → reached zero
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    *pte = 0;
}
```

#### 5.2 `vma_free_all` (`vma.c`)

Unchanged at the VMA level — it calls `vmm_unmap_4k_page` in a loop, which
now handles COW pages correctly.

For 2MB huge pages (not yet split), `vma_free_all` checks
`page_cow_put(phys_2m)` and only calls `free_pages` when the count reaches zero.

#### 5.3 `do_exit` / `sys_exec` cleanup (`task.c`)

The existing `vma_free_all(current->mm)` call (do_exit L419, sys_exec L1025)
handles the COW cleanup automatically via the modified `vmm_unmap_4k_page`.

#### 5.4 `do_munmap`

`do_munmap` already calls `vmm_unmap_4k_page` in a loop — COW-aware
automatically.

### 6. `do_mprotect` Adaptation (`vma.c`)

When `mprotect` transitions a PTE from read-only to writable:
- If the PTE has `PAGE_COW` set and `page_cow_refs(phys) == 1`:
  clear `PAGE_COW`, set `PAGE_R_W` (in-place promotion, no copy)
- If the PTE has `PAGE_COW` set and `page_cow_refs(phys) > 1`:
  allocate a new page + copy + update PTE + `page_cow_put(old_phys)`
  (same logic as COW fault)

When `mprotect(PROT_NONE)`:
- If the PTE has `PAGE_COW` set, call `page_cow_put(phys)` before clearing
  the PTE (release our COW reference). This is critical — otherwise the
  physical page's cow_count is inflated for a mapping that no longer exists.

When `mprotect(PROT_READ)` on a PROTNONE page: restore the PTE from
`PAGE_PROTNONE` but check whether the page is COW-shared and preserve
`PAGE_COW`.

### 7. SMP Safety (Racy-but-Correct)

Two cores writing to the same COW-shared page simultaneously may both
enter the COW fault handler. Each sees `cow_count >= 2` and allocates
a new page. The second allocator's old_phys `cow_count` decrements to 0
and frees the physical page. The first allocator's copy is used by its
core. This is **correct** — both cores get isolated pages.  The only
waste is an extra allocation + copy on one core (which is freed).

This race is rare in practice (cores writing to the exact same 4KB within
the same tick windows) and the overhead is bounded.  Full atomicity would
require per-4KB-page locks or CAS on the PTE, which is deferred to V2.

The current `alloc_4k_page` / `free_4k_page` already hold
`spin_lock_irqsave(&subpage_lock)` internally.

### 8. Testing

Existing tests: `test/mmap_test.c` has fork+mmap isolation tests (commit
`473fabc`).

New tests to add:

| Test | Description |
|------|-------------|
| `cow_basic` | fork → child writes → verify parent unchanged → parent writes → verify child unchanged |
| `cow_exec` | fork → child execs → parent writes → verify no crash (last-reference in-place restore) |
| `cow_2mb_split` | mmap 2MB anonymous → fork → both write to different 4KB slots → verify isolation |
| `cow_mprotect` | fork → child mprotect(PROT_READ) → child mprotect(PROT_WRITE) → verify COW on write |
| `cow_oom` | Exhaust 4KB pages → fork → child writes → verify SIGSEGV clean (no kernel panic) |
| `cow_exit` | fork → child exits → parent writes → verify in-place restore |

## Omitted from V1

| Item | Rationale |
|------|-----------|
| Atomic PTE CAS | Racy-but-correct is acceptable for the rare dual-write race |
| cow_sub reclamation | When all 512 sub-counts reach 0, cow_sub could be kfree'd. V1 leaks the 1024-byte array (per split page, rare). |
| PMD (2MB) COW without split | Splitting to 4KB on every COW fault is correct. Direct 2MB COW (without split) is an optimization for when the entire 2MB range is COW-faulted — defer. |
| 1GB page support | No 1GB pages exist in current mappings. |

## File Change Summary

| File | Change | ~Lines |
|------|--------|--------|
| `kernel/include/kernel/vmm.h` | Add `PAGE_COW` macro | 2 |
| `kernel/include/kernel/pmm.h` | Add `cow_count`, `cow_sub` to `struct Page`; declare `phys_to_page` + `page_cow_*` helpers | 20 |
| `kernel/memory/pmm.c` | Implement `page_cow_get/put/refs` (lazy cow_sub alloc); `phys_to_page` | 40 |
| `kernel/sched/task.c` | Rewrite `fork_mm_copy` from eager copy to COW sharing | 60 |
| `kernel/arch/x86_64/trap.c` | Add COW branch to `do_page_fault` | 40 |
| `kernel/memory/vmm.c` | Add `split_2mb_to_4k`; update `vmm_unmap_4k_page` for COW | 65 |
| `kernel/memory/vma.c` | Update `do_mprotect` for COW-awareness | 20 |
| `test/mmap_test.c` | Add 6 COW-specific test cases | 50 |
| **Total** | | **~297** |
