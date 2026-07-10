# COW (Copy-on-Write) Fork — Design Specification

> Date: 2026-07-10  (v4: 4KB-only Path 2 after third review)
> Status: approved
> Scope: kernel-only, ~245 lines, 7 files

## Motivation

`fork_mm_copy` currently does EAGER copying: every writable user page is
fully duplicated via `alloc_pages`/`alloc_4k_page` + `memcpy`. In the common
`fork+exec` pattern (shell spawning a command), the child process immediately
discards these copies via `exec`.  Fork latency scales with heap size rather
than being bounded by page-table walk time.

COW (Copy-on-Write) fixes this: fork shares 4KB pages read-only between
parent and child; the first writer triggers a page fault that allocates only
the 4KB page actually being written.

## Scope: 4KB-only V1

V1 applies COW **only to 4KB PTE-table leaves** (VMA-managed pages from
`alloc_4k_page`: anonymous mmap, brk heap, file-backed mmap).

**2MB huge-page PDE leaves** (ELF segments, user stack mapped via
`vmm_map_page`) are **not COW-tracked** — the existing eager copy in
`fork_mm_copy` is preserved for these entries.

### Why not 2MB COW in V1?

1. **No VMA coverage**: ELF segments and the user stack are mapped via
   `vmm_map_page(..., PAGE_USER_Page)` without `vma_insert`.  A COW-fault
   on a 2MB PDE would hit `vma_find → NULL → kill_current_user_task` in
   `do_page_fault` before reaching any COW branch.  Adding VMA coverage
   for all 2MB regions is a larger change than COW itself.

2. **No subpage-pool tracking**: 2MB pages come from `alloc_pages` and are
   not tracked by any `subpage_pool`.  If split to 4KB PTEs, `free_4k_page`
   would be a silent no-op on the resulting sub-pages (they are not in any
   pool's bitmap).  Proper split-page lifecycle requires deeper PMM changes.

3. **Low marginal benefit**: 2MB eager copy costs ~100μs per 2MB leaf
   (rep movsb).  In fork+exec the child immediately execs, so the copy
   is wasted — but the 100μs is negligible compared to the ~100ms of
   copying a large heap's worth of 4KB pages.  4KB COW captures the
   bulk of the performance win with far less risk.

## Design: Page-Level COW via subpage_pool.cow_count

Per-4KB-slot reference counts live in `struct subpage_pool` (not `struct
Page` — 2MB frames are not COW-tracked).  A PTE software bit `PAGE_COW`
(bit 10) tags shared PTEs.

Fork: clear `PAGE_R_W` + set `PAGE_COW` on every writable 4KB PTE, and
increment the slot's `cow_count` in the containing subpage_pool.

Fault handler:
- `cow_count > 1` → alloc_4k_page + memcpy + decrement old ref
- `cow_count == 1` → restore `PAGE_R_W` in-place (zero-copy)

Exit/unmap: decrement `cow_count`, free the 4KB page only when it reaches 0.

## Components

### 1. Data Structures

#### 1.1 New PTE software bit (`vmm.h`)

```c
#define PAGE_COW  (1UL << 10)  // bit 10: COW-shared, write triggers fault
```

Bit 10 in x86-64 PTEs is ignored by hardware (available to software).
Bit 9 is already used by `PAGE_PROTNONE`.

A COW-shared PTE carries `PAGE_U_S | PAGE_Present | PAGE_COW` (R/W=0).
A write triggers #PF with error_code bit 1 set.

#### 1.2 `struct subpage_pool` extension (`pmm.c`)

```c
struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    int         alloc_count;
    uint16_t    cow_count[SUBPAGE_4K_COUNT];  // ← NEW: per-4KB-slot COW refcount
};
```

`sizeof(subpage_pool)` grows by 1024 bytes (512 × uint16_t).  Each pool
backs a 2MB frame; at one pool this is negligible.

**No changes to `struct Page`** — 2MB frames are never COW-tracked in V1.

#### 1.3 Helper functions (`pmm.c` + `pmm.h`)

All three functions are implemented in `pmm.c` and declared in `pmm.h`.
They internally acquire `subpage_lock` (static to pmm.c, IRQ-safe spinlock
already used by `alloc_4k_page`/`free_4k_page`).

```c
// Increment the COW refcount for a 4KB physical page.
// Walks subpage_pools to find the containing pool, then bumps cow_count[slot].
// Caller must hold no locks; subpage_lock is acquired internally.
void     page_cow_get(uint64_t phys);

// Decrement the COW refcount for a 4KB physical page.
// Returns true when cow_count[slot] reaches 0 (caller may free the page).
bool     page_cow_put(uint64_t phys);

// Read the current COW refcount for a 4KB physical page.
uint16_t page_cow_refs(uint64_t phys);
```

Internal implementation:
```c
void page_cow_get(uint64_t phys) {
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        pool->cow_count[slot]++;
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
}
// page_cow_put and page_cow_refs follow the same pattern.
```

#### 1.4 alloc_4k_page: zero cow_count on allocation

```c
// Inside alloc_4k_page, after finding/allocating a slot:
pool->cow_count[slot] = 0;   // fresh page starts with zero COW refs
```

This prevents stale cow_count values from surviving across free+realloc
cycles (M1 fix).

#### 1.5 free_4k_page: no changes needed

`free_4k_page` only touches `bitmap` and `alloc_count`.  `cow_count[slot]`
is left as-is — it will be zeroed on next `alloc_4k_page` of that slot (§1.4).
No correctness issue (the slot is marked free in bitmap; cow_count is dead
until reallocated).

### 2. `fork_mm_copy`: 2MB Eager, 4KB COW

The existing function is split into two leaf-type paths:

#### 2.1 2MB huge-page PDE leaf (`pml2[l2] & PAGE_PS`)

**Unchanged** — eager copy via `alloc_pages` + `rep movsb`.  No PAGE_COW
bits are set on 2MB PDEs.  These pages are private to the child and never
trigger a COW fault.

```c
// Existing code preserved as-is:
if (pml2e & PAGE_PS) {
    // ... alloc_pages + rep movsb (task.c:1149-1166) ...
}
```

#### 2.2 4KB PTE-table leaf (`!(pml2e & PAGE_PS)`)

The existing eager-copy loop (task.c:1132-1158) is replaced with COW sharing.
**PAGE_COW is checked before PAGE_R_W** — a COW-shared page already has
`PAGE_R_W` cleared, and checking `PAGE_R_W` first would misclassify it as
plain read-only, skipping `page_cow_get` (fork-of-fork use-after-free).

```c
if (!(pml2e & PAGE_PS)) {
    // Deep-copy the PTE table itself
    uint64_t *parent_pte = Phy_To_Virt(pml2e & PAGE_4K_MASK);
    uint64_t *child_pte  = calloc(1, PAGE_4K_SIZE);
    if (!child_pte) { child_pml2[l2] = pml2e; continue; } // OOM: share
    child_pml2[l2] = Virt_To_Phy((uint64_t)child_pte)
                   | (pml2e & 0xfff);

    for (int l1 = 0; l1 < 512; l1++) {
        uint64_t pte = parent_pte[l1];
        if (!(pte & (PAGE_Present | PAGE_PROTNONE)))
            continue;

        // Priority: COW before R/W
        if (pte & PAGE_COW) {
            // Already COW-shared (parent was itself forked)
            page_cow_get(pte & PAGE_4K_MASK);
            child_pte[l1] = pte;
        } else if (pte & PAGE_R_W) {
            // Path A: writable → mark COW on BOTH parent and child
            parent_pte[l1] &= ~PAGE_R_W;
            parent_pte[l1] |= PAGE_COW;          // parent in-place R/O+COW
            page_cow_get(pte & PAGE_4K_MASK);
            child_pte[l1] = parent_pte[l1];
        } else {
            // Path B: plain read-only → share directly, no tracking
            child_pte[l1] = pte;
        }
    }
}
```

#### 2.3 TLB invalidation

`flush_tlb()` (local TLB flush, **not** `tlb_shootdown()`).  The modified
PTEs are per-process user entries; only the current CPU can have stale
writable TLB entries for the parent's address space.  Other CPUs do not
run the parent's mm (OS01 has no same-mm multi-threading).

### 3. `do_page_fault`: COW Fault Handling

The COW branch is inserted **after** the existing write-to-readonly-VMA check
(trap.c:465) and **before** the existing P=0 demand-paging path (trap.c:480).
This ordering is critical: a COW page's VMA has `VM_WRITE` set, so it passes
the write-permission check and reaches the COW branch.  A genuine write to a
read-only non-COW page is caught by the existing check first.

Only 4KB PTEs are handled.  2MB PDEs never carry `PAGE_COW` and thus never
enter this branch.

```c
// P=1, W=1  (write to a present page → possible COW)
// Placed AFTER the VM_WRITE permission check (trap.c:465)
// and BEFORE the P=0 demand-paging path (trap.c:480).
if ((error_code & 0x03) == 0x03) {

    task_t *t = task_from_tss();   // IST stack — do NOT use current
    uint64_t *user_pml4 = Phy_To_Virt((uint64_t)t->mm->pml4);

    uint64_t *pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
    if (!pte || !(*pte & PAGE_COW)) {
        kill_current_user_task(regs);
        return;
    }

    uint64_t old_phys = *pte & PAGE_4K_MASK;

    if (page_cow_refs(old_phys) > 1) {
        // Multiple sharers — allocate + copy
        uint64_t new_phys = alloc_4k_page();
        if (!new_phys) { kill_current_user_task(regs); return; }
        memcpy((void *)Phy_To_Virt(new_phys),
               (void *)Phy_To_Virt(old_phys), PAGE_4K_SIZE);

        *pte = new_phys | vma->vm_page_prot;
        page_cow_put(old_phys);
    } else {
        // Last reference — restore writable in-place, zero copy
        *pte = old_phys | vma->vm_page_prot;
    }
    flush_tlb();
    return;
}
```

### 4. Exit / Exec / Munmap: Decrement cow_count

#### 4.1 `vmm_unmap_4k_page` (`vmm.c`)

```c
void vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt) {
    uint64_t *pte = vmm_pt_walk(pagemap, virt, 0, 0);
    if (!pte) return;
    if (!(*pte & (PAGE_Present | PAGE_PROTNONE))) return;

    uint64_t phys = *pte & PAGE_4K_MASK;

    if (*pte & PAGE_COW) {
        if (page_cow_put(phys))  // returns true → count hit zero
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    *pte = 0;
}
```

#### 4.2 `vma_free_all` (`vma.c`)

Unchanged — it loops over `vmm_unmap_4k_page`, which now handles COW pages.

#### 4.3 `vmm_free_user_map` (`vmm.c`)

**Unchanged** — 2MB PDE leaves are never COW-tagged.  Existing eager-copy
cleanup logic remains correct.

#### 4.4 `do_exit` and `sys_exec` cleanup (`task.c`)

Unchanged.  `vma_free_all(current->mm)` handles 4KB COW pages automatically
via the modified `vmm_unmap_4k_page`.  2MB pages are handled (or leaked, as
pre-existing) by `vmm_free_user_map` — no COW impact.

### 5. `do_mprotect` Adaptation (`vma.c`)

#### 5.1 R/O → R/W transition on a COW page

```c
if (*pte & PAGE_COW) {
    uint64_t phys = *pte & PAGE_4K_MASK;
    if (page_cow_refs(phys) > 1) {
        uint64_t new_phys = alloc_4k_page();
        memcpy(Phy_To_Virt(new_phys), Phy_To_Virt(phys), PAGE_4K_SIZE);
        page_cow_put(phys);
        *pte = new_phys | new_page_prot;
    } else {
        *pte = phys | new_page_prot;   // last reference, promote in-place
    }
}
```

#### 5.2 `mprotect(PROT_NONE)` on a COW page

Keep the COW reference.  Stash the PTE as PROTNONE while preserving
PAGE_COW and the physical address:

```c
if (prot == PROT_NONE && (*pte & PAGE_COW)) {
    *pte = phys | PAGE_U_S | PAGE_PROTNONE | PAGE_COW;
    // Do NOT page_cow_put — another process still shares this page.
}
```

#### 5.3 `mprotect(PROT_READ)` restoring a PROTNONE page

```c
if (*pte & PAGE_PROTNONE) {
    uint64_t phys = *pte & PAGE_4K_MASK;
    if (*pte & PAGE_COW) {
        if (page_cow_refs(phys) == 1) {
            *pte = phys | new_page_prot;   // last sharer → promote to R/W
        } else {
            *pte = phys | PAGE_U_S | PAGE_Present | PAGE_COW;  // still shared
        }
    } else {
        *pte = phys | new_page_prot;
    }
}
```

### 6. SMP Safety

#### 6.1 cow_count atomicity

`page_cow_get/put/refs` are protected by `subpage_lock` (IRQ-safe spinlock).
The lock serialises all read-modify-write sequences on `cow_count[slot]`.
The caller of `page_cow_put` who sees `true` is the sole owner — safe to
call `free_4k_page`.

#### 6.2 Dual COW fault on the same 4KB slot

Two CPUs COW-faulting the same 4KB slot: `subpage_lock` serialises the
`page_cow_refs/put` sequence.  If count was 2, both see ≥2, both allocate
+ copy + `page_cow_put`.  The first put takes count 2→1, the second takes
1→0 and frees old_phys.  Both CPUs have private copies — correct isolation.
One extra allocation+free cycle on the second CPU.

Note: this requires both CPUs to run threads of the same mm, which OS01
does not currently support (no same-mm multi-threading).  The lock-based
approach is sufficient; the analysis is included for completeness.

#### 6.3 Fork-of-fork correctness

The `PAGE_COW`-before-`PAGE_R_W` check in §2.2 ensures chain-fork works:

```
P1 fork→P2:  page X (4KB, R/W) → R/O+COW, cow_count=2
P1 fork→P3:  page X: PAGE_COW checked first → page_cow_get → cow_count=3
P2 write→X:  refs=3>1 → alloc+copy+put → refs=2
P3 write→X:  refs=2>1 → alloc+copy+put → refs=1
P1 write→X:  refs=1 → in-place R/W  ✓
```

### 7. Testing

New tests to add to `test/mmap_test.c`:

| Test | Description |
|------|-------------|
| `cow_basic` | fork → child writes → verify parent unchanged → parent writes → verify child unchanged |
| `cow_exec` | fork → child execs → parent writes → verify no crash (last-reference in-place restore) |
| `cow_fork_of_fork` | P1 fork→P2 fork→P3 → P2 writes → P3 writes → P1 writes → verify all isolated |
| `cow_mprotect` | fork → child mprotect(PROT_NONE) → mprotect(PROT_READ) → verify COW preserved |
| `cow_oom` | Exhaust 4KB pages → fork → child writes → verify SIGSEGV clean (no kernel panic) |
| `cow_exit` | fork → child exits → parent writes → verify in-place restore |

### 8. Omitted from V1

| Item | Rationale |
|------|-----------|
| 2MB COW (huge-page splitting) | Requires VMA coverage for ELF/stack regions + subpage-pool tracking for split pages. 4KB COW captures the bulk of fork+exec performance benefit. |
| cow_sub / per-2MB-frame tracking | Not needed — 2MB pages are never split to 4KB.  All 4KB COW tracking lives in subpage_pool. |
| Atomic PTE CAS | Lock-based cow_count is sufficient. PTE is only written by the faulting CPU. |
| Exit-time 2MB ELF page leak | Pre-existing (task.c:422). Not introduced or worsened by COW. |

## File Change Summary

| File | Change | ~Lines |
|------|--------|--------|
| `kernel/include/kernel/vmm.h` | Add `PAGE_COW` macro | 2 |
| `kernel/include/kernel/pmm.h` | Declare `page_cow_get`, `page_cow_put`, `page_cow_refs` | 10 |
| `kernel/memory/pmm.c` | Add `cow_count[512]` to `subpage_pool`; implement helpers; zero cow_count in `alloc_4k_page` | 55 |
| `kernel/sched/task.c` | Rewrite 4KB PTE loop in `fork_mm_copy` to COW sharing (2MB path unchanged); `flush_tlb` | 60 |
| `kernel/arch/x86_64/trap.c` | Add COW branch to `do_page_fault` (after VM_WRITE check, before P=0 path) | 35 |
| `kernel/memory/vmm.c` | Update `vmm_unmap_4k_page` for COW-aware free | 15 |
| `kernel/memory/vma.c` | Update `do_mprotect` for COW (R/O→R/W + PROT_NONE + restore) | 25 |
| `test/mmap_test.c` | Add 6 COW tests | 50 |
| **Total** | | **~252** |
