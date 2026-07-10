# COW (Copy-on-Write) Fork — Design Specification

> Date: 2026-07-10  (revised v3 after second review)
> Status: approved
> Scope: kernel-only, ~390 lines, 8 files

## Motivation

`fork_mm_copy` currently does EAGER copying: every writable 2MB/4KB user page
is fully duplicated via `alloc_pages`/`alloc_4k_page` + `memcpy`. In the common
`fork+exec` pattern (shell spawning a command), the child process immediately
discards these copies.  Fork latency scales with process size (~100ms for a 2MB
data segment) instead of being bounded by page-table walk time (~1μs).

## Design: Page-Level COW via cow_count

**Approach A**: per-physical-page reference count (`cow_count`), tagged in the
PTE with `PAGE_COW` (bit 10).  Fork clears `PAGE_R_W` on every writable PTE,
sets `PAGE_COW`, and increments the backing physical page's `cow_count`.
The fault handler checks `cow_count`:
- `> 1` → allocate new page + copy + decrement old ref
- `== 1` → restore `PAGE_R_W` in-place (zero-copy — last-sharer path)

Tracking is unified via `struct Page`.  `phys_to_page(phys)` works for every
physical address in the system (`PMMngr.pages_struct` is indexed by 2MB frame
number), so there is no separate pool-level bookkeeping.

- **2MB huge pages**: `page->cow_count` tracks the shared count.  On first COW
  fault the page is split to 4KB (§4); after split, `cow_sub[]` takes over.
- **4KB pages** (from `alloc_4k_page` or post-split): `page->cow_sub[slot]`
  tracks the shared count. `cow_sub` is lazily allocated on first use
  (1 KB per 2MB frame that has COW-shared 4KB pages).

### Why not other approaches?

**Approach B (mm-level refcount)**: Parent and child have independent PML4 +
PTEs. A mm-level counter can't track which physical page is still shared —
child COW-faulting a page updates only its own PTE, leaving the parent's PTE
pointing at the old phys.  Leads to leaks or dangling PTEs.

**Approach C (blind-free on COW fault)**: Without a refcount, the fault handler
must always either leak the old page (if another process shares it) or free it
(risking a dangling PTE in the other process).  Neither is correct.

## Components

### 1. Data Structures

#### 1.1 New PTE software bit (`vmm.h`)

```c
#define PAGE_COW  (1UL << 10)  // bit 10: COW-shared, write triggers fault
```

Bit 10 in x86-64 PTEs is ignored by hardware (available to software).
Bit 9 is already used by `PAGE_PROTNONE`.

A COW-shared PTE carries `PAGE_U_S | PAGE_Present | PAGE_COW` (R/W=0).
A write to such a PTE triggers #PF with error_code bit 1 set (write fault).

#### 1.2 `struct Page` extension (`pmm.h`)

```c
struct Page {
    // ... existing fields unchanged ...
    uint16_t  cow_count;   // COW reference count (2MB granularity before split)
    uint16_t *cow_sub;     // NULL until split: 512-element per-4KB-slot counts
};
```

`sizeof(struct Page)` grows by **16 bytes**: `uint16_t cow_count` at offset 40
(2 bytes), then 6 bytes padding for pointer alignment, then `cow_sub` (8 bytes).
Total: 40 + 2 + 6 + 8 = **56 bytes**.  At 1 GB RAM, pages_struct has
512 entries × 56 B ≈ **28 KB** — acceptable.

**⚠️  `make clean` required** — `sizeof(struct Page)` changes, and stale `.o`
files with mismatched sizes cause silent ABI bugs (AGENTS.md).

**No changes to `subpage_pool`** — 4KB-page COW tracking goes through
`phys_to_page(phys_4k)->cow_sub[slot]`, not a separate pool-level array.

#### 1.3 Helper functions (`pmm.c` + `pmm.h`)

```c
// Map any physical address to its struct Page*.
// Works for both 2MB (huge-page) and 4KB (sub-page) addresses:
//   idx = phys >> PAGE_2M_SHIFT
//   return &PMMngr.pages_struct[idx]
//
// NOTE: for a 4KB sub-page, this returns the 2MB frame's Page, NOT a
// per-4KB struct.  Callers must use cow_sub[slot] for sub-page counts.
static inline struct Page *phys_to_page(uint64_t phys);

// ── COW refcount helpers ────────────────────────────────
// All three acquire subpage_lock internally (IRQ-safe spinlock).
//
// Routing:
//   pg->cow_sub != NULL  → cow_sub[slot]   (4KB or already-split 2MB)
//   pg->cow_sub == NULL  → cow_count        (unsplit 2MB huge page)
//
// slot = (phys >> PAGE_4K_SHIFT) & 0x1FF

void     page_cow_get(uint64_t phys);   // ++cow_count
bool     page_cow_put(uint64_t phys);   // --cow_count; returns true when it reaches 0
uint16_t page_cow_refs(uint64_t phys);  // read current cow_count
```

#### 1.4 Concurrency contract

`page_cow_get`, `page_cow_put`, `page_cow_refs` each acquire `subpage_lock`
(IRQ-safe spinlock already used by `alloc_4k_page`/`free_4k_page` in
`pmm.c`).  This makes all three operations atomic with respect to each other
and to the 4KB allocator — critical for correctness of `page_cow_put`'s
"return true if count reaches zero → caller may free" contract.

`cow_sub` array access is also guarded by `subpage_lock` (the lock is held
across the `cow_sub != NULL` check and the slot increment/decrement).

### 2. `fork_mm_copy`: Share, Don't Copy

The existing eager-copy loop is replaced wholesale.  The new code walks the
parent's PML4[0:256] and for each writable leaf copies the PTE but with
`PAGE_R_W` cleared and `PAGE_COW` set — sharing the physical page rather than
duplicating it.  There are **two leaf types**.

**Critical ordering**: the PTE classification checks `PAGE_COW` **before**
`PAGE_R_W`.  A COW-shared page already has `PAGE_R_W` cleared — checking
`PAGE_R_W` first would misclassify it as plain read-only and skip
`page_cow_get`.  This would cause a use-after-free in subsequent fork-of-fork
scenarios (see §7.3 for the full failure sequence).

#### 2.1 2MB huge-page PDE leaf (`pml2[l2] & PAGE_PS`)

```c
// Priority order: COW before R/W.  A COW page has R/W already cleared.
if (pml2e & PAGE_COW) {
    // Already COW-shared (parent was itself forked) — bump the existing ref
    page_cow_get(pml2e & PAGE_2M_MASK);
    child_pml2[l2] = pml2e;                    // copy R/O+COW PDE as-is
} else if (pml2e & PAGE_R_W) {
    // Path A: writable page → mark COW on BOTH parent and child
    parent_pml2[l2] &= ~PAGE_R_W;              // parent: in-place R/O
    parent_pml2[l2] |= PAGE_COW;
    page_cow_get(pml2e & PAGE_2M_MASK);        // new COW reference
    child_pml2[l2] = parent_pml2[l2];           // child: copy of R/O+COW PDE
} else {
    // Path B: plain read-only (e.g. .text, .rodata) — share, no tracking
    child_pml2[l2] = pml2e;
}
```

#### 2.2 4KB PTE-table leaf (PDE points to a PTE table, i.e. `!(pml2e & PAGE_PS)`)

The existing code (task.c:1132-1158) allocates a new PTE table and eagerly
copies every 4KB page.  This must be converted to COW sharing with the same
PAGE_COW-before-PAGE_R_W ordering:

```c
// PDE is a 4KB PTE table pointer (not a 2MB huge page)
if (!(pml2e & PAGE_PS)) {
    // Deep-copy the PTE table itself (512 × 8 = 4KB)
    uint64_t *parent_pte = Phy_To_Virt(pml2e & PAGE_4K_MASK);
    uint64_t *child_pte  = calloc(1, PAGE_4K_SIZE);
    // ... wire child_pte into child_pml2 ...

    for (int l1 = 0; l1 < 512; l1++) {
        uint64_t pte = parent_pte[l1];
        if (!(pte & (PAGE_Present | PAGE_PROTNONE)))
            continue;

        if (pte & PAGE_COW) {
            // Already COW-shared → bump the existing ref
            page_cow_get(pte & PAGE_4K_MASK);
            child_pte[l1] = pte;
        } else if (pte & PAGE_R_W) {
            // Path A: writable 4KB page → COW share
            parent_pte[l1] &= ~PAGE_R_W;
            parent_pte[l1] |= PAGE_COW;         // parent in-place R/O+COW
            page_cow_get(pte & PAGE_4K_MASK);
            child_pte[l1] = parent_pte[l1];
        } else {
            // Path B: plain read-only → share directly
            child_pte[l1] = pte;
        }
    }
}
```

#### 2.3 TLB invalidation

At the end of `fork_mm_copy`: `flush_tlb()` (local TLB flush, **not**
`tlb_shootdown()`).  The modified PTEs are per-process user entries; only the
current CPU — which is executing fork() on behalf of the parent — can have
stale writable TLB entries for the parent's address space.  Other CPUs do not
run the parent's mm (OS01 has no multi-threaded same-mm support), so an IPI
broadcast is unnecessary.

### 3. `do_page_fault`: COW Fault Handling

#### 3.1 Detecting a 2MB huge-page fault

`vmm_pt_walk` returns NULL when `pml2[l2] & PAGE_PS` is set (vmm.c:226).
To detect a 2MB COW page we must therefore resolve the PDE **before** calling
`vmm_pt_walk`.  The approach: hand-walk PML4→PML3→PML2, check the PDE, and
only call `vmm_pt_walk` after confirming the entry is a 4KB PTE table.

**IST stack note**: `do_page_fault` runs on the IST stack.  `get_current_task()`
uses `RSP & ~(STACK_SIZE-1)` which is wrong on IST stacks.  The existing code
(trap.c:443) uses `task_from_tss()` — the COW branch must do the same.

```c
// P=1, W=1  (write to a present page → possible COW or permission violation)
if ((error_code & 0x03) == 0x03) {

    // ── Get task via TSS (NOT current — IST stack) ───
    task_t *t = task_from_tss();
    if (!t || !t->mm)
        goto cow_sigsegv;

    // ── Hand-walk to PDE to detect 2MB huge pages ──────
    size_t l4 = (cr2 >> 39) & 0x1FF;
    size_t l3 = (cr2 >> 30) & 0x1FF;
    size_t l2 = (cr2 >> 21) & 0x1FF;

    uint64_t *user_pml4 = Phy_To_Virt((uint64_t)t->mm->pml4);
    if (!(user_pml4[l4] & PAGE_Present))            goto cow_sigsegv;
    uint64_t *pml3 = Phy_To_Virt(user_pml4[l4] & PAGE_4K_MASK);
    if (!(pml3[l3] & PAGE_Present))                  goto cow_sigsegv;
    uint64_t *pml2 = Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);

    uint64_t pde = pml2[l2];
    if (!(pde & PAGE_Present))                       goto cow_sigsegv;

    // ── 2MB huge page?  Split first ──
    if (pde & PAGE_PS) {
        if (!(pde & PAGE_COW))                       goto cow_sigsegv;
        split_2mb_to_4k(user_pml4, cr2, pde & PAGE_2M_MASK, pde);
        // After split: PDE is now a 4KB PTE table — re-walk
    }

    // ── Now safe to call vmm_pt_walk (guaranteed 4KB PTE at leaf) ──
    uint64_t *pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
    if (!pte || !(*pte & PAGE_COW))                  goto cow_sigsegv;

    uint64_t old_phys = *pte & PAGE_4K_MASK;

    if (page_cow_refs(old_phys) > 1) {
        // Multiple sharers — allocate + copy
        uint64_t new_phys = alloc_4k_page();
        if (!new_phys) { kill_current_user_task(regs); return; }
        memcpy((void *)Phy_To_Virt(new_phys),
               (void *)Phy_To_Virt(old_phys), PAGE_4K_SIZE);

        *pte = new_phys | vma->vm_page_prot;
        page_cow_put(old_phys);   // drop our share of the old page
    } else {
        // Last reference — restore writable in-place, zero copy.
        // Clear PAGE_COW (leave PAGE_R_W as-is — it's already 0, and
        // vma->vm_page_prot will set it below).  Actually simpler:
        *pte = old_phys | vma->vm_page_prot;   // includes R/W, U/S, XD
    }
    flush_tlb();
    return;

cow_sigsegv:
    kill_current_user_task(regs);
    return;
}
```

The existing P=0 demand-paging path (VM_ANON / VM_FILE) is unchanged.

Note: `split_2mb_to_4k` is passed the raw PDE so it can inherit PAGE_XD and
other high-level flags (§4).

### 4. `split_2mb_to_4k`: 2MB Huge Page → 4KB PTEs

When a COW-fault hits a 2MB huge-page PDE, split the 2MB entry into
512 4KB PTEs, each R/O + COW.  Lives in **pmm.c** (needs `subpage_lock`,
which is static to pmm.c).  Thread-safe: `subpage_lock` + CAS guards the
cow_sub allocation race.

```c
// pmm.c — requires subpage_lock (static in this file)
// pde is the FULL PDE value (with flags), not just phys_2m — caller passes
// it so we can inherit PAGE_XD, PAGE_U_S, PAGE_PAT from the original 2MB PDE.
void split_2mb_to_4k(uint64_t *user_pml4, uint64_t fault_va,
                     uint64_t phys_2m, uint64_t orig_pde) {
    struct Page *pg = phys_to_page(phys_2m);

    // ── Allocate cow_sub under lock to prevent duplicate alloc ──
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    if (!pg->cow_sub) {
        uint16_t *sub = calloc(512, sizeof(uint16_t));
        if (!sub) {
            spin_unlock_irqrestore(&subpage_lock, flags);
            return;  // OOM — caller will SIGSEGV
        }
        // Broadcast 2MB cow_count to all 512 per-slot entries
        for (int i = 0; i < 512; i++)
            sub[i] = pg->cow_count;
        // Publish via CAS — second racer sees non-NULL, skips
        if (!__sync_bool_compare_and_swap(&pg->cow_sub, NULL, sub)) {
            kfree(sub);  // we lost the race
        }
        // cow_count now delegated to cow_sub[]; zero the 2MB field
        pg->cow_count = 0;
    }
    spin_unlock_irqrestore(&subpage_lock, flags);

    // ── Walk to PDE ──
    size_t l4 = (fault_va >> 39) & 0x1FF;
    size_t l3 = (fault_va >> 30) & 0x1FF;
    size_t l2 = (fault_va >> 21) & 0x1FF;
    uint64_t *pml3  = Phy_To_Virt(user_pml4[l4] & PAGE_4K_MASK);
    uint64_t *pml2  = Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);
    uint64_t *pde   = &pml2[l2];

    // ── Build PTE table ──
    // Preserve high-level flags from the original PDE: PAGE_XD (bit 63),
    // PAGE_U_S (bit 2), PAGE_PAT (bit 12).  Without PAGE_XD, a non-executable
    // stack region (task.c:771: PAGE_USER_Page | PAGE_XD) would become
    // executable after split — a security regression.
    uint64_t pte_flags = (orig_pde & (PAGE_XD | PAGE_U_S | PAGE_PAT))
                       | PAGE_Present | PAGE_COW;

    uint64_t *pte_table = calloc(1, PAGE_4K_SIZE);
    if (!pte_table) return;

    for (int i = 0; i < 512; i++) {
        pte_table[i] = (phys_2m + i * PAGE_4K_SIZE) | pte_flags;
        // R/W=0 so writes trigger COW fault at 4KB granularity
    }

    // Replace PDE: was 2MB huge page, now a 4KB PTE table pointer
    *pde = Virt_To_Phy((uint64_t)pte_table) | PAGE_USER_Dir;

    flush_tlb();
}
```

After this, `vmm_pt_walk` succeeds and returns a `uint64_t *pte`.  The COW
fault handler proceeds with the 4KB path.

**Overhead per split**: 1024 bytes (`cow_sub`) + 4096 bytes (PTE table) ≈ 5 KB.
Only pages that are actually written post-fork are split; `.text` / `.rodata`
never split.

**Known limitation**: a 2MB page allocated via `alloc_pages` (not
`alloc_4k_page`) is not tracked by any `subpage_pool`.  After split, its
4KB sub-pages are mapped via PTEs and unmap/write-fault paths will call
`free_4k_page(phys)`, which searches the subpage_pool list and finds nothing
— the call is a silent no-op.  These sub-pages leak indefinitely.  Not a
regression (the exit path already leaks 2MB pages pre-COW, task.c:422), but
documented here for debug traceability.

### 5. Exit / Exec / Munmap: Decrement cow_count

#### 5.1 `vmm_unmap_4k_page` (`vmm.c`)

4KB PTE leaf — same API, COW-aware logic:

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

#### 5.2 `vmm_free_user_map` — 2MB huge-page COW cleanup (`vmm.c`)

`vma_free_all` loops at 4KB granularity via `vmm_unmap_4k_page`, which
cannot reach 2MB PDE leaves (`vmm_pt_walk` returns NULL for PAGE_PS).
2MB huge pages are freed by `vmm_free_user_map` (vmm.c:137-181), called
from `spawn_user_task` (on ELF load error path) and from `sys_exec` (on
old-mm teardown, currently commented out).  This function must become
COW-aware:

```c
// Inside vmm_free_user_map, the 2MB leaf loop (vmm.c:166-171):
if (pml2e & PAGE_PS) {
    uintptr_t phys = pml2e & (PAGE_2M_MASK & ~PAGE_XD);
    struct Page *page = Phy_to_2M_Page(phys);

    if (pml2e & PAGE_COW) {
        // COW-shared 2MB page — decrement, only free when count hits 0
        if (page_cow_put(phys)) {
            page_clean(page);
            free_pages(page, 1);
        }
    } else {
        // Non-COW 2MB page — free unconditionally (existing behaviour)
        page_clean(page);
        free_pages(page, 1);
    }
}
```

**Call sites that trigger `vmm_free_user_map`**:
- `spawn_user_task` (task.c:753, 769) — ELF load error path
- `sys_exec` (task.c:915, 929) — ELF load error path
  (The `sys_exec` L1025 `vma_free_all` call handles 4KB VMA pages only.)

**Note**: The `do_exit` path currently skips `vmm_free_user_map` entirely
(task.c:422 "Skip vmm_free_user_map for now") — 2MB ELF pages leak on exit.
This is a pre-existing issue, not introduced by COW.  The COW implementation
does not change this; see §9 for the performance impact.

#### 5.3 `vma_free_all` (`vma.c`)

Unchanged — it calls `vmm_unmap_4k_page` in a loop, which now handles COW
4KB pages correctly (§5.1).  2MB pages are out of scope for this function.

### 6. `do_mprotect` Adaptation (`vma.c`)

#### 6.1 R/O → R/W transition on a COW page

Same logic as COW fault (§3):

```c
if (pte & PAGE_COW) {
    uint64_t phys = *pte & PAGE_4K_MASK;
    if (page_cow_refs(phys) > 1) {
        uint64_t new_phys = alloc_4k_page();
        memcpy(Phy_To_Virt(new_phys), Phy_To_Virt(phys), PAGE_4K_SIZE);
        page_cow_put(phys);
        *pte = new_phys | new_page_prot;  // no PAGE_COW on new page
    } else {
        // Last reference: remove COW, use VMA's R/W prot
        *pte = phys | new_page_prot;
    }
}
```

#### 6.2 `mprotect(PROT_NONE)` on a COW page

**Keep the COW reference.**  Stash the PTE as PROTNONE while preserving the
PAGE_COW tag and physical address:

```c
if (prot == PROT_NONE && (*pte & PAGE_COW)) {
    // Hide the PTE but keep our COW share alive — do NOT page_cow_put.
    // The physical page is still referenced by another process; if we
    // released our ref, the other process could see count==1 and
    // incorrectly promote to writable (in-place, sharing our write).
    *pte = phys | PAGE_U_S | PAGE_PROTNONE | PAGE_COW;
}
```

On `mprotect(PROT_READ)` restoring a PROTNONE page:

```c
// Restore a stashed PTE
if (*pte & PAGE_PROTNONE) {
    uint64_t phys = *pte & PAGE_4K_MASK;
    if (*pte & PAGE_COW) {
        // Still COW-shared — restore as R/O + COW.
        // If cow_count==1 (other sharer exited), promote to writable now.
        if (page_cow_refs(phys) == 1) {
            *pte = phys | new_page_prot;  // R/W, no COW
        } else {
            *pte = phys | PAGE_U_S | PAGE_Present | PAGE_COW;
        }
    } else {
        *pte = phys | new_page_prot;
    }
}
```

### 7. SMP Safety

#### 7.1 cow_count atomicity

`page_cow_get/put/refs` are protected by `subpage_lock` (IRQ-safe
spinlock, pmm.c:80).  The lock serialises all read-modify-write sequences
on cow_count / cow_sub[slot].  The caller of `page_cow_put` who sees a
return value of `true` is guaranteed to be the sole owner of the physical
page — safe to call `free_4k_page` / `free_pages`.

#### 7.2 split_2mb_to_4k race

Two CPUs may COW-fault the same 2MB page simultaneously.  Both enter
`split_2mb_to_4k`.  The `subpage_lock` + `__sync_bool_compare_and_swap`
on `pg->cow_sub` ensures only one `calloc`'d array is installed; the
loser's array is `kfree`'d.  Both CPUs then independently build their
PTE tables (harmless — same content, same phys_2m, same R/O+COW).
The last writer's PDE wins; both are equivalent.

#### 7.3 Dual COW fault on the same 4KB slot

Two CPUs COW-faulting the same 4KB slot concurrently: `subpage_lock`
serialises the `page_cow_refs/put` sequence.  If count was 2, both see ≥2,
both allocate + copy + `page_cow_put`.  The first put takes count 2→1,
the second takes 1→0 and frees old_phys.  Both CPUs now have private copies
— correct isolation.  One extra allocation + free cycle on the second CPU.

#### 7.4 Fork-of-fork correctness

The PAGE_COW-before-PAGE_R_W ordering in §2 ensures that chain-fork works:

```
P1 fork→P2:  page X: R/W → R/O+COW, cow_count=2
P1 fork→P3:  page X: PAGE_COW checked first → page_cow_get → cow_count=3
P2 write→X:  refs=3>1 → alloc+copy→refs=2  (P1,P3 still share)
P3 write→X:  refs=2>1 → alloc+copy→refs=1  (P1 only)
P1 write→X:  refs=1 → in-place R/W          (zero copy, correct)
```

Without the PAGE_COW check, at step "P1 fork→P3" Path B would be taken,
cow_count would stay at 2, and P3's later write would see refs=1 (after
P2's write) and incorrectly promote to writable — sharing P1's page.

### 8. Testing

Existing tests: `test/mmap_test.c` has fork+mmap isolation tests (commit
`473fabc`).

New tests:

| Test | Description |
|------|-------------|
| `cow_basic` | fork → child writes → verify parent unchanged → parent writes → verify child unchanged |
| `cow_exec` | fork → child execs → parent writes → verify no crash (last-reference in-place restore) |
| `cow_2mb_split` | mmap 2MB anonymous → fork → both write to different 4KB slots → verify isolation |
| `cow_fork_of_fork` | P1 fork→P2 fork→P3 → P2 writes → P3 writes → P1 writes → verify all isolated |
| `cow_mprotect` | fork → child mprotect(PROT_NONE) → child mprotect(PROT_READ) → verify COW preserved |
| `cow_oom` | Exhaust 4KB pages → fork → child writes → verify SIGSEGV clean (no kernel panic) |
| `cow_exit` | fork → child exits → parent writes → verify in-place restore |

### 9. Omitted from V1

| Item | Rationale |
|------|-----------|
| cow_sub reclamation | 1 KB leak per split page. Only occurs when a page is written post-fork — rare in fork+exec. |
| Atomic PTE CAS | Lock-based cow_count is sufficient. PTE is only written by the faulting CPU's own page table. |
| Direct 2MB COW (without split) | Splitting to 4KB is correct. Direct 2MB COW is an optimisation to avoid the split overhead. |
| 1GB page support | No 1GB pages in current mappings. |
| Exit-time 2MB ELF page leak | Pre-existing (task.c:422). Does cause cow_count to stay inflated for 2MB pages when a parent exits without writing — the surviving child will always see refs≥2 and never get the in-place promote optimisation. This is a performance regression on the leak path only, not a correctness bug. |
| Split 2MB sub-page free (free_4k_page) | `alloc_pages` 2MB pages are not in any subpage_pool; free_4k_page on their split sub-pages is a silent no-op. Permanent leak of split sub-pages — acceptable since the exit path already leaks the whole 2MB page pre-COW. |

## File Change Summary

| File | Change | ~Lines |
|------|--------|--------|
| `kernel/include/kernel/vmm.h` | Add `PAGE_COW` macro | 2 |
| `kernel/include/kernel/pmm.h` | Add `cow_count`, `cow_sub` to `struct Page`; declare `phys_to_page`, `page_cow_*`, `split_2mb_to_4k` | 28 |
| `kernel/memory/pmm.c` | Implement `page_cow_get/put/refs` (subpage_lock); `phys_to_page`; `split_2mb_to_4k` (CAS + PAGE_XD preservation) | 90 |
| `kernel/sched/task.c` | Rewrite `fork_mm_copy`: 2MB+4KB COW sharing with PAGE_COW-before-PAGE_R_W ordering, local flush_tlb | 85 |
| `kernel/arch/x86_64/trap.c` | Add COW branch to `do_page_fault` (hand-walk PDE, task_from_tss, split call, 4KB COW resolve) | 55 |
| `kernel/memory/vmm.c` | Update `vmm_unmap_4k_page` + `vmm_free_user_map` for COW | 30 |
| `kernel/memory/vma.c` | Update `do_mprotect` for COW-awareness (R/O→R/W + PROT_NONE stash + restore) | 35 |
| `test/mmap_test.c` | Add 7 COW-specific test cases (including fork-of-fork) | 65 |
| **Total** | | **~390** |
