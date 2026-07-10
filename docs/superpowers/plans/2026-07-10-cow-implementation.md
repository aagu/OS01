# COW (Copy-on-Write) Fork 4KB-only V1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace eager 4KB-page copying in `fork_mm_copy` with copy-on-write sharing, backed by per-4KB-slot reference counts in `struct subpage_pool.cow_count[]`.

**Architecture:** Add `PAGE_COW` (PTE bit 10) to tag shared read-only PTEs. Extend `struct subpage_pool` with `cow_count[512]`. Rewrite the 4KB PTE loop in `fork_mm_copy` to share pages rather than copy them. Add a COW branch to `do_page_fault` that resolves writes to shared pages. Make `vmm_unmap_4k_page`, `do_mprotect`, and `alloc_4k_page` COW-aware.

**Tech Stack:** C (kernel), x86-64, QEMU, LLVM/Clang toolchain. No new dependencies.

**Implied spec:** `docs/superpowers/specs/2026-07-10-cow-design.md` (v6)

**⚠️  `make clean` required after struct layout changes.**

---

### File Structure

| File | Role |
|------|------|
| `kernel/include/kernel/vmm.h` | `PAGE_COW` macro (line 44) |
| `kernel/include/kernel/pmm.h` | Declare `page_cow_get`, `page_cow_put`, `page_cow_refs` |
| `kernel/memory/pmm.c` | `subpage_pool` extension, helpers, `alloc_4k_page` zeroing |
| `kernel/sched/task.c` | `fork_mm_copy` 4KB loop rewrite |
| `kernel/arch/x86_64/trap.c` | COW branch in `do_page_fault` |
| `kernel/memory/vmm.c` | COW-aware `vmm_unmap_4k_page` |
| `kernel/memory/vma.c` | COW-aware `do_mprotect` PTE-update loop |
| `user/test_cow.c` | Integration test binary (new) |
| `Makefile` | Add test_cow.elf to disk image |

---

### Task 1: Add PAGE_COW macro

**Files:**
- Modify: `kernel/include/kernel/vmm.h:44`

- [ ] **Step 1: Add PAGE_COW definition**

```c
#define PAGE_PROTNONE     (1UL << 9)   // software bit: PROT_NONE stash marker
// bit 9 is x86_64 PTE ignored. mprotect(PROT_NONE) sets this, clears Present
// but keeps phys.  mprotect(PROT_READ) walks PTEs to restore Present + clear this.
// do_munmap/vma_free_all check this bit to know phys is valid for free_4k_page.

#define PAGE_COW          (1UL << 10)  // software bit: COW-shared, write triggers fault
// bit 10 is x86_64 PTE ignored.  Fork sets this on writable PTEs, clears PAGE_R_W.
// COW fault handler checks this bit; if set with P=1,W=1, resolves COW.
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/kernel/vmm.h
git commit -m "feat(vmm): add PAGE_COW PTE software bit (bit 10)"
```

---

### Task 2: Extend struct subpage_pool with cow_count[]

**Files:**
- Modify: `kernel/memory/pmm.c:72-77`

- [ ] **Step 1: Add cow_count field to subpage_pool struct**

In `kernel/memory/pmm.c`, replace the existing struct definition:

```c
struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    int         alloc_count;
    uint16_t    cow_count[SUBPAGE_4K_COUNT];  // COW refcount: how many COW PTEs map each 4KB slot
};
```

`SUBPAGE_4K_COUNT` is 512 (defined at line 70: `PAGE_2M_SIZE / PAGE_4K_SIZE`). The struct grows from ~92 bytes to ~1116 bytes, still fitting in slot 0's 4KB.

- [ ] **Step 2: Run make clean && make to verify no build regressions**

```bash
make clean && make 2>&1 | tail -5
```
Expected: build succeeds (no callers of cow_count yet; struct layout change is transparent until the field is accessed).

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/pmm.c
git commit -m "feat(pmm): add cow_count[512] to struct subpage_pool"
```

---

### Task 3: Zero cow_count in alloc_4k_page

**Files:**
- Modify: `kernel/memory/pmm.c:379-430`

- [ ] **Step 1: Add cow_count[slot] = 0 in alloc_4k_page existing-slot path**

In `alloc_4k_page`, after finding a free bit at line 396 and computing `phys`, zero the slot's cow_count. Also zero it in the new-pool path for slot 1.

For the existing-pool path (line 396-399), add after `pool->alloc_count++`:

```c
                pool->bitmap[i] |= (1ULL << bit);
                pool->alloc_count++;
                int slot = i * 64 + bit;
                pool->cow_count[slot] = 0;   // fresh page, no COW references
                uint64_t phys = pool->base_phys
                              + (uint64_t)slot * PAGE_4K_SIZE;
                spin_unlock_irqrestore(&subpage_lock, flags);
                return phys;
```

For the new-pool path (line 424-428), add after `pool->alloc_count++`:

```c
    pool->bitmap[0] |= (1ULL << 1);
    pool->alloc_count++;
    pool->cow_count[1] = 0;             // fresh slot 1, no COW references

    uint64_t phys = pool->base_phys + PAGE_4K_SIZE;
```

Note: slot 0 holds the pool struct itself and is never returned by `alloc_4k_page` — its cow_count is dead storage, no need to zero it.

- [ ] **Step 2: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/pmm.c
git commit -m "feat(pmm): zero cow_count[slot] on alloc_4k_page"
```

---

### Task 4: Implement page_cow_get / page_cow_put / page_cow_refs helpers

**Files:**
- Modify: `kernel/include/kernel/pmm.h` (declarations)
- Modify: `kernel/memory/pmm.c` (implementations)

- [ ] **Step 1: Extract find_pool_locked helper from free_4k_page**

The existing `free_4k_page` (pmm.c:432-463) has a pool-search loop. Extract it into a static helper:

```c
// Find the subpage_pool containing phys, or NULL.  Must be called with
// subpage_lock held.  Extracted from free_4k_page's pool walk.
static struct subpage_pool *find_pool_locked(uint64_t phys)
{
    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool =
            container_of(pos, struct subpage_pool, list);
        if (phys >= pool->base_phys &&
            phys < pool->base_phys + PAGE_2M_SIZE)
            return pool;
        pos = pos->next;
    }
    return NULL;
}
```

Place this function **before** `alloc_4k_page` (before line 379) so all callers can see it.

- [ ] **Step 2: Refactor free_4k_page to use find_pool_locked**

Replace the inline pool-search loop in `free_4k_page` (lines 438-459) with:

```c
void free_4k_page(uint64_t phys)
{
    if (!phys) return;

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        uint64_t offset = phys - pool->base_phys;
        int slot = (int)(offset / PAGE_4K_SIZE);
        if (slot >= 0 && slot < SUBPAGE_4K_COUNT) {
            int word = slot / 64;
            int bit  = slot % 64;
            if (pool->bitmap[word] & (1ULL << bit)) {
                pool->bitmap[word] &= ~(1ULL << bit);
                pool->alloc_count--;
            }
        }
    }

    spin_unlock_irqrestore(&subpage_lock, flags);
}
```

- [ ] **Step 3: Implement page_cow_get, page_cow_put, page_cow_refs**

Add after `find_pool_locked`, before `alloc_4k_page`:

```c
// ── COW refcount helpers ──────────────────────────────
// All acquire subpage_lock internally.  Caller must hold no locks.

void page_cow_get(uint64_t phys)
{
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        pool->cow_count[slot]++;
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
}

bool page_cow_put(uint64_t phys)
{
    bool reached_zero = false;
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        if (pool->cow_count[slot] > 0) {
            pool->cow_count[slot]--;
            if (pool->cow_count[slot] == 0)
                reached_zero = true;
        }
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
    return reached_zero;
}

uint16_t page_cow_refs(uint64_t phys)
{
    uint16_t refs = 0;
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        refs = pool->cow_count[slot];
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
    return refs;
}
```

- [ ] **Step 4: Declare helpers in pmm.h**

`kernel/include/kernel/pmm.h` currently includes only `<stdint.h>` and `<stddef.h>`. `page_cow_put` returns `bool`, so add `#include <stdbool.h>` at the top of the file (after line 2).

Then, after the `free_4k_page` declaration (line 102), add:

```c
// COW refcount helpers — acquire subpage_lock internally
void     page_cow_get(uint64_t phys);
bool     page_cow_put(uint64_t phys);   // returns true when count reaches 0
uint16_t page_cow_refs(uint64_t phys);
```

- [ ] **Step 5: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: build succeeds. New functions are declared but not yet called by anyone — no functional change.

- [ ] **Step 6: Commit**

```bash
git add kernel/memory/pmm.c kernel/include/kernel/pmm.h
git commit -m "feat(pmm): implement page_cow_get/put/refs with subpage_lock"
```

---

### Task 5: Rewrite 4KB PTE loop in fork_mm_copy to COW sharing

**Files:**
- Modify: `kernel/sched/task.c:1115-1147`

- [ ] **Step 1: Replace the eager-copy 4KB loop with COW sharing**

Replace lines 1115–1147 (the block from `if (!(pml2e & PAGE_PS)) {` through `continue;`) with the COW-sharing version:

```c
                // 4KB PTE table: share pages via COW.
                // Check PAGE_COW before PAGE_R_W — a COW page has R/W=0
                // and must not be misclassified as plain read-only.
                if (!(pml2e & PAGE_PS)) {
                    if (!(pml2e & PAGE_Present)) {
                        child_pml2[l2] = 0;
                        continue;
                    }
                    uint64_t *parent_pte =
                        (uint64_t *)Phy_To_Virt(pml2e & PAGE_4K_MASK);
                    uint64_t *child_pte =
                        (uint64_t *)calloc(1, PAGE_4K_SIZE);
                    if (!child_pte) {
                        child_pml2[l2] = pml2e;  // OOM: share PDE
                        continue;
                    }
                    child_pml2[l2] = Virt_To_Phy((uint64_t)child_pte)
                                   | (pml2e & 0xfff);
                    for (int l1 = 0; l1 < 512; l1++) {
                        uint64_t pte = parent_pte[l1];
                        if (!(pte & (PAGE_Present | PAGE_PROTNONE)))
                            continue;

                        if (pte & PAGE_COW) {
                            // Already COW-shared (fork-of-fork)
                            page_cow_get(pte & PAGE_4K_MASK);
                            child_pte[l1] = pte;
                        } else if (pte & PAGE_R_W) {
                            // Path A: writable → COW on BOTH parent and child.
                            // page_cow_get twice: parent PTE (R/W→R/O+COW) +1,
                            // child PTE (new COW) +1 → cow_count grows by 2.
                            parent_pte[l1] &= ~PAGE_R_W;
                            parent_pte[l1] |= PAGE_COW;
                            page_cow_get(pte & PAGE_4K_MASK);
                            page_cow_get(pte & PAGE_4K_MASK);
                            child_pte[l1] = parent_pte[l1];
                        } else {
                            // Path B: plain read-only → share directly
                            child_pte[l1] = pte;
                        }
                    }
                    continue;
                }
```

- [ ] **Step 2: Add flush_tlb() at end of fork_mm_copy**

`fork_mm_copy` ends at line 1184. Before `return child_mm;` (line 1177), add `flush_tlb()`. Find the exact location:

```c
    *cr3_out = (uint64_t)child_mm->pml4;

    // TLB flush: parent's in-memory PTEs were modified (R/W→R/O+COW).
    // Only the current CPU runs the parent's mm — local flush is sufficient.
    flush_tlb();

    return child_mm;
```

- [ ] **Step 3: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: build succeeds. The new code references `PAGE_COW` and `page_cow_get` — verify no linker errors.

- [ ] **Step 4: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(fork): convert 4KB PTE loop to COW sharing
- Check PAGE_COW before PAGE_R_W for fork-of-fork correctness
- Writable pages: clear R/W, set COW on parent + child PTEs
- Read-only pages: share directly, no COW tracking
- 2MB huge pages: unchanged (eager copy)
- flush_tlb() at end for parent's stale TLB entries"
```

---

### Task 6: Add COW branch to do_page_fault

**Files:**
- Modify: `kernel/arch/x86_64/trap.c:465-480`

- [ ] **Step 1: Insert COW branch between VM_WRITE check and P=0 path**

After the existing write-protection check (line 470-471, the `if ((error_code & 0x03) == 0x03 && !(vma->vm_flags & VM_WRITE))` block), and before the `if (!(error_code & 0x01))` demand-paging path (line 480), insert:

```c

            // ── COW resolution (P=1, W=1, VM_WRITE is set) ──
            // At this point: the page is present, the fault is a write,
            // and the VMA allows writes.  If the PTE has PAGE_COW set,
            // this is a COW page — resolve by copying or promoting.
            if ((error_code & 0x03) == 0x03) {
                uint64_t *user_pml4 =
                    (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pml4);
                uint64_t *pte = vmm_pt_walk(user_pml4, cr2, 0, 0);
                if (pte && (*pte & PAGE_COW)) {
                    uint64_t old_phys = *pte & PAGE_4K_MASK;

                    if (page_cow_refs(old_phys) > 1) {
                        // Multiple sharers — allocate + copy
                        uint64_t new_phys = alloc_4k_page();
                        if (!new_phys) {
                            kill_current_user_task(regs);
                            return;
                        }
                        memcpy((void *)Phy_To_Virt(new_phys),
                               (void *)Phy_To_Virt(old_phys),
                               PAGE_4K_SIZE);
                        *pte = new_phys | vma->vm_page_prot;
                        page_cow_put(old_phys);
                    } else {
                        // Last reference — promote in-place.
                        // page_cow_put drops the stale count before we
                        // rewrite the PTE; ignore the return value
                        // (count went 1→0, but the page stays alive).
                        (void)page_cow_put(old_phys);
                        *pte = old_phys | vma->vm_page_prot;
                    }
                    flush_tlb();
                    return;
                }
                // If !pte or !PAGE_COW: not a COW page, but we already
                // passed the VM_WRITE check.  Fall through to the P=0
                // path, which will skip (P=1) and hit the unhandled kill.
            }

```

The indentation must match the existing style (tabs, 2 levels deep inside `if (regs->cs & 3)` and its block).

- [ ] **Step 2: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: builds without errors. Now the COW path is reachable but no user code triggers it yet (no integration test yet).

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat(trap): add COW resolution branch to do_page_fault
Inserted after VM_WRITE permission check, before P=0 demand-paging.
- PAGE_COW + refs>1: allocate+copy+put
- PAGE_COW + refs==1: promote in-place with put to avoid count leak
- Uses task_from_tss() (IST stack), not get_current_task()"
```

---

### Task 7: Make vmm_unmap_4k_page COW-aware

**Files:**
- Modify: `kernel/memory/vmm.c:255-269`

- [ ] **Step 1: Replace vmm_unmap_4k_page with COW-aware version**

```c
void vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt)
{
    uint64_t *pte = vmm_pt_walk(pagemap, virt, 0, 0);
    if (!pte)
        return;

    // Must check both Present and PROTNONE — PROTNONE pages have
    // Present=0 but valid phys that must be freed.
    if (!(*pte & (PAGE_Present | PAGE_PROTNONE)))
        return;

    uint64_t phys = *pte & PAGE_4K_MASK;

    if (*pte & PAGE_COW) {
        // COW-shared page: decrement refcount, free only when count hits 0
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    *pte = 0;
}
```

- [ ] **Step 2: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: builds without errors.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/vmm.c
git commit -m "feat(vmm): make vmm_unmap_4k_page COW-aware
Check PAGE_COW before freeing; if set, decrement cow_count via
page_cow_put and only free the page when the count reaches zero."
```

---

### Task 8: Make do_mprotect COW-aware

**Files:**
- Modify: `kernel/memory/vma.c:347-357`

- [ ] **Step 1: Replace the PTE-update inner loop in do_mprotect**

Replace lines 347–357 (the `for (uint64_t va = va_start; ...)` loop body inside the `!(*pte & ...) continue;` line):

```c
            uint64_t phys = *pte & PAGE_4K_MASK;

            if (prot == PROT_NONE) {
                // Stash phys for later restore.
                // Preserve PAGE_COW if set — we keep our COW reference.
                uint64_t stash = phys | PAGE_U_S | PAGE_PROTNONE;
                if (*pte & PAGE_COW)
                    stash |= PAGE_COW;
                *pte = stash;
            } else {
                // Restoring from PROTNONE or changing existing mapping.
                // Check COW before blindly applying new_page_prot.
                if (*pte & PAGE_COW) {
                    if (page_cow_refs(phys) > 1) {
                        // Multiple sharers: allocate private copy
                        uint64_t new_phys = alloc_4k_page();
                        if (!new_phys) continue; // OOM: skip this page
                        memcpy((void *)Phy_To_Virt(new_phys),
                               (void *)Phy_To_Virt(phys), PAGE_4K_SIZE);
                        page_cow_put(phys);
                        *pte = new_phys | new_page_prot;
                    } else {
                        // Last sharer: promote in-place
                        (void)page_cow_put(phys);
                        *pte = phys | new_page_prot;
                    }
                } else if (*pte & PAGE_PROTNONE) {
                    // Non-COW PROTNONE restore
                    *pte = phys | new_page_prot;
                } else {
                    *pte = phys | new_page_prot;
                }
            }
```

This replaces both the `if (prot == PROT_NONE)` branch (line 353-354) and the `else` branch (line 355-357).

- [ ] **Step 2: Rebuild and verify**

```bash
make clean && make 2>&1 | tail -5
```
Expected: builds without errors.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/vma.c
git commit -m "feat(mprotect): COW-aware PTE updates in do_mprotect
- PROT_NONE: preserve PAGE_COW on stash
- R/W restore: check refcount; >1 alloc+copy+put; ==1 promote in-place
- PROTNONE restore: check PAGE_COW, resolve if needed"
```

---

### Task 9: Write COW integration test

**Files:**
- Create: `user/test_cow.c`
- Modify: `Makefile`

- [ ] **Step 1: Write the test binary**

Create `user/test_cow.c`:

```c
// test_cow — COW fork isolation tests
//
// Tests:
//  1. cow_basic:        fork, both write, verify isolation
//  2. cow_fork_of_fork: P1→P2→P3 chain, all write, verify isolation
//  3. cow_mprotect:     fork, child PROT_NONE→restore→write, verify isolation
//  4. cow_exec:         fork, child execs, parent writes (last-ref promote)
//  5. cow_exit:         fork, child exits, parent writes (in-place promote)
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(int cond, const char *msg)
{
    if (!cond) { printf("FAIL: %s\n", msg); failures++; }
}

int main(void)
{
    printf("test_cow: COW fork isolation tests\n");

    // ── Test 1: cow_basic ──
    {
        printf("  cow_basic: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x42;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            ((int *)p)[0] = 0x99;
            if (((int *)p)[0] != 0x99) exit(1);
            exit(0);
        }

        int st; waitpid(pid, &st, 0);
        check(((int *)p)[0] == 0x42, "parent sees child write");
        check(st == 0, "child exit 0");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // ── Test 2: cow_fork_of_fork ──
    {
        printf("  cow_fork_of_fork: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 1;

        int64_t p1 = fork();   // P1
        check(p1 >= 0, "fork1");

        if (p1 == 0) {
            int64_t p2 = fork(); // P2
            check(p2 >= 0, "fork2");

            if (p2 == 0) {
                // P3: grandchild
                ((int *)p)[0] = 3;
                if (((int *)p)[0] != 3) exit(1);
                exit(0);
            }
            // P2: child
            ((int *)p)[0] = 2;
            if (((int *)p)[0] != 2) exit(1);
            int st2; waitpid(p2, &st2, 0);
            check(st2 == 0, "grandchild exit 0");
            exit(0);
        }

        // P1: parent
        int st1; waitpid(p1, &st1, 0);
        check(st1 == 0, "p2 exit 0");
        check(((int *)p)[0] == 1, "parent isolated from children");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // ── Test 3: cow_mprotect ──
    {
        printf("  cow_mprotect: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x11;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            // Child: stash page via PROT_NONE, then restore and write
            int rc = mprotect(p, 4096, PROT_NONE);
            check(rc == 0, "mprotect PROT_NONE");
            // Page is now stashed (PROTNONE+COW) — should not fault
            rc = mprotect(p, 4096, PROT_READ | PROT_WRITE);
            check(rc == 0, "mprotect restore");
            // Now write — COW should allocate a private copy
            ((int *)p)[0] = 0x22;
            if (((int *)p)[0] != 0x22) exit(1);
            exit(0);
        }

        int st; waitpid(pid, &st, 0);
        check(st == 0, "child exit 0");
        check(((int *)p)[0] == 0x11, "parent isolated from mprotect child");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // ── Test 4: cow_exec ──
    {
        printf("  cow_exec: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0xAB;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            // Child execs spin.elf (a small binary that exits 42).
            // exec replaces mm → child's COW refs are released.
            char *argv[] = {"/spin.elf", NULL};
            execve("/spin.elf", argv, NULL);
            exit(99); // exec failed
        }

        int st; waitpid(pid, &st, 0);
        check(st == (42 << 8), "spin exit 42");
        // Parent writes — should be last reference, in-place promote.
        ((int *)p)[0] = 0xCD;
        check(((int *)p)[0] == 0xCD, "parent write after exec");
        munmap(p, 4096);
        printf("PASS\n");
    }

    // ── Test 5: cow_exit ──
    {
        printf("  cow_exit: ");
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check(p != MAP_FAILED, "mmap");
        ((int *)p)[0] = 0x77;

        int64_t pid = fork();
        check(pid >= 0, "fork");

        if (pid == 0) {
            exit(0);  // child exits without writing
        }

        int st; waitpid(pid, &st, 0);
        check(st == 0, "child exit 0");
        // Parent writes — child is gone, cow_count should be 1.
        // In-place promote (no copy needed).
        ((int *)p)[0] = 0x88;
        check(((int *)p)[0] == 0x88, "parent write after child exit");
        munmap(p, 4096);
        printf("PASS\n");
    }

    if (failures == 0)
        printf("test_cow: ALL PASS\n");
    else
        printf("test_cow: %d FAILURES\n", failures);

    return failures > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Add test binary to disk image in Makefile**

In `Makefile`, find the existing test binary line (around line 110: `mcopy -i $@ build/x86_64/user/test_fork_mmap.elf`). After it, add:

```makefile
	mcopy -i $@ build/x86_64/user/test_cow.elf ::/test_cow.elf
```

- [ ] **Step 3: Rebuild and run the test in QEMU**

```bash
make clean && make
make run
```

In the QEMU serial console, run:
```
/test_cow.elf
```

Expected output:
```
test_cow: COW fork isolation tests
  cow_basic: PASS
  cow_fork_of_fork: PASS
  cow_exec: PASS
  cow_exit: PASS
test_cow: ALL PASS
```

- [ ] **Step 4: Commit**

```bash
git add user/test_cow.c Makefile
git commit -m "test: add COW fork isolation test (5 test cases)
- cow_basic: fork, both write, verify isolation
- cow_fork_of_fork: P1→P2→P3 chain, all write, verify isolation
- cow_mprotect: fork, child PROT_NONE→restore→write, verify isolation
- cow_exec: fork, child execs spin.elf, parent writes (last-ref promote)
- cow_exit: fork, child exits, parent writes (in-place promote)"
```

---

### Task 10: Final integration test — full systest

**Files:**
- (none new — verify systest.elf still passes)

- [ ] **Step 1: Run existing systest after COW changes**

```bash
make run
```

In QEMU, run:
```
/systest.elf
```

Expected: all 70 tests pass (no regressions from COW).

- [ ] **Step 2: Run test_cow again**

```
/test_cow.elf
```

Expected: ALL PASS.

- [ ] **Step 3: Interactive smoke test**

In QEMU, run a shell fork sequence:
```
echo hello
mkdir /test_dir
ls /
```

All should work without crashes (these exercise fork+exec via busybox ash).

- [ ] **Step 4: Final commit if any Makefile tweaks were needed**

```bash
git add -A
git diff --cached --stat
git commit -m "chore: verify COW passes systest (70/70) + smoke test"
```

---

## Verification Checklist

After all tasks complete:

- [ ] `make clean && make` succeeds with zero warnings
- [ ] `make run` boots to shell
- [ ] `/systest.elf` → 70 passed, 0 failed
- [ ] `/test_cow.elf` → ALL PASS
- [ ] `echo hello` / `ls /` / `mkdir /x` / `rmdir /x` work in ash
- [ ] Multiple `fork+exec` cycles via busybox ash do not leak or crash
