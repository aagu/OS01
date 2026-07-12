# COW Fork & Memory Mapping (V1)

## 4KB page support

The kernel defaults to 2MB huge pages (`PML4 → PDPT → PDE` with the `PS` bit set) for
both kernel and user space.  To support fine-grained COW sharing and `mmap`, a
**subpage pool** in `kernel/memory/pmm.c` subdivides 2MB pages into 512 × 4KB slots.

Each `subpage_pool` (`pmm.c:72`) tracks one 2MB physical page:

```c
struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[8];        // 512 bits = 1 bit per 4KB slot
    int         alloc_count;
    uint16_t    cow_count[512];   // COW refcount per slot
};
```

- `bitmap[8]` — 512 bits, one per slot.  Bit N = 1 → slot N is allocated.
- `cow_count[512]` — how many PTE references (across parent and children) map
  each 4KB slot via COW.  Used for `fork`-of-`fork` and leaf‑child promotion.
- Pools are linked in the global `subpage_pools` list, protected by `subpage_lock`.

**API** (all in `pmm.c`):

| Function | Purpose |
|----------|---------|
| `alloc_4k_page()` | Finds a free slot in an existing pool, or allocates a new 2MB pool page and returns slot 1 (slot 0 holds the pool struct itself).  Returns physical address or 0. |
| `free_4k_page(phys)` | Clears the bitmap bit for `phys`, decrements `alloc_count`.  No-op if `phys==0`. |
| `page_cow_get(phys)` | Increments `cow_count[slot]`. |
| `page_cow_put(phys)` | Decrements `cow_count[slot]`, returns `true` when it reaches 0. |
| `page_cow_refs(phys)` | Returns current `cow_count[slot]` (for debugging / COW decisions). |

**Exclusion:** framebuffer pages are allocated outside the subpage pool and never
managed by these helpers.

---

## COW (Copy-on-Write) fork

### Page table flag

```c
#define PAGE_COW  (1UL << 10)   // kernel/include/kernel/vmm.h:50
```

Bit 10 is an x86‑64 **ignored bit** (hardware never interprets it).  A COW page
has `R/W=0`, `PAGE_COW=1`: writes trap to `#PF`, and the fault handler checks
`PAGE_COW` to distinguish COW from a plain read‑only fault.

### Fork: `fork_mm_copy()` in `kernel/sched/task.c:1075`

Called from `do_fork()`.  Creates child page tables and populates them:

1. **Copy kernel half:** entries 256–511 of the child PML4 are `memcpy`'d from
   `init_mm.pml4` (shared kernel mapping).
2. **Walk user PML4 (entries 0–255):** for each present entry, allocate a child
   PDPT → for each present PDPT entry, allocate a child PD (page directory).
3. **For each PD entry (PML2E):**

   | PTE type | Action |
   |----------|--------|
   | **2MB huge page** (`PAGE_PS` set) | Eager copy: allocate a fresh 2MB page, `rep movsb` the content.  No COW for huge pages in V1. |
   | **4KB PTE table** (no `PAGE_PS`) | Allocate a child PTE table, then iterate 512 entries: |
   | &nbsp;&nbsp; `PAGE_COW` already set | **Fork‑of‑fork:** `page_cow_get`, child PTE = parent PTE. |
   | &nbsp;&nbsp; Writable (`PAGE_R_W`) | **COW install:** clear `R/W`, set `PAGE_COW` on **parent** PTE; `page_cow_get` ×2 (one for parent, one for child); child PTE = parent PTE. |
   | &nbsp;&nbsp; Read‑only (no `PAGE_R_W`) | **Share directly:** child PTE = parent PTE, no COW tracking. |

4. **Deep‑copy VMA list** via `fork_vma_copy()` (`vma.c:90`).
5. **TLB flush** — parent's in‑memory PTEs were modified, a local flush is
   sufficient since only the current CPU runs the parent.

### COW fault resolution: `do_page_fault()` in `kernel/arch/x86_64/trap.c:400`

When a user‑mode write hits a COW page (`#PF` with error code `0x03` = present +
write), the handler at `trap.c:449` checks `PAGE_COW`:

```c
if (pte && (*pte & PAGE_COW)) {
    uint64_t old_phys = *pte & PAGE_4K_MASK;
    if (page_cow_refs(old_phys) > 1) {
        // Multiple sharers: allocate new page, copy content, decrement count
        uint64_t new_phys = alloc_4k_page();
        memcpy(Phy_To_Virt(new_phys), Phy_To_Virt(old_phys), PAGE_4K_SIZE);
        *pte = new_phys | vma->vm_page_prot;
        page_cow_put(old_phys);
    } else {
        // Last sharer: promote in-place
        page_cow_put(old_phys);
        *pte = old_phys | vma->vm_page_prot;
    }
    flush_tlb();
}
```

- **`cow_count > 1`:** allocate a private 4KB page, copy old content, install
  the new page with full R/W permissions, decrement the old page's refcount.
- **`cow_count == 1`:** (this task is the sole remaining COW referencer)
  promote the existing page in‑place — set `R/W`, clear `PAGE_COW`, refcount→0.
  No allocation or copy needed.

### `vmm_unmap_4k_page` COW awareness (`kernel/memory/vmm.c:256`)

```c
if (*pte & PAGE_COW) {
    if (page_cow_put(phys))
        free_4k_page(phys);
} else {
    free_4k_page(phys);
}
```

A COW page is only freed when the last COW reference (`cow_count`) drops to 0.
Without this check, unmapping a COW page in a child would free shared physical
memory still mapped by the parent.

### `do_mprotect` COW awareness (`kernel/memory/vma.c:312`)

- **`PROT_NONE` stash:** preserves `PAGE_COW` when storing the physical address
  as `PROTNONE` — the COW reference is kept alive.
- **Restore from `PROTNONE`:** if `PAGE_COW` is set, re‑resolves COW just like
  the fault handler: copy if `cow_count > 1`, promote if `cow_count == 1`.

---

## VMA (Virtual Memory Area) subsystem

**Files:** `kernel/memory/vma.c`, `kernel/include/kernel/task.h` (`mm_t`)

### Data structures

```c
// kernel/include/kernel/task.h:68
typedef struct mm_struct {
    uint64_t *pml4;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;      // sorted by vm_start
    uint64_t mmap_base;     // start search address for mmap
} mm_t;

// kernel/memory/vma.c (vma_t, defined locally)
typedef struct vma {
    list_t      list;
    uint64_t    vm_start, vm_end;
    uint64_t    vm_flags;       // VM_READ, VM_WRITE, VM_EXEC, VM_SHARED, VM_ANON
    uint64_t    vm_page_prot;   // PTE flags (PAGE_USER_4K, etc.)
    uint64_t    vm_pgoff;       // file offset in 4KB units
    vfs_node_t *vm_file;        // mapped file (NULL for anonymous)
} vma_t;
```

### Operations

| Function | Purpose |
|----------|---------|
| `vma_insert(mm, vma)` | Insert VMA into sorted VMA list by `vm_start`. No merge (VMA count < 20). |
| `vma_remove(mm, vma)` | Remove VMA node, release file ref. Does NOT free pages. |
| `vma_free_all(mm)` | Unmap all 4KB pages in every VMA via `vmm_unmap_4k_page`, then remove and free all VMA nodes. Called by exec/exit. |

### `do_mmap(addr, len, prot, flags, fd, offset)` — `vma.c:215`

1. **Validate** arguments (overflow, alignment, file existence for non‑anonymous).
2. **Find a hole** in the VMA space starting from `mm->mmap_base`.  Walks the
   sorted VMA list looking for a gap large enough.  For `MAP_FIXED`, calls
   `do_munmap` to clear the region first.
3. **Allocate VMA** and insert it into `vma_list`.
4. **Returns** the mapped address or `MAP_FAILED` (`-1`).

No physical pages are allocated at `mmap` time — they are demand‑paged on first
access via `do_page_fault` (anonymous: `alloc_4k_page`, file‑backed: `vfs_read`).

### `do_munmap(addr, len)` — `vma.c:156`

1. Aligns to 4KB boundaries.
2. Walks the VMA list, finds overlapping VMAs.
3. For each overlapped range: unmap all 4KB PTEs via `vmm_unmap_4k_page` (COW‑aware).
4. Handles VMA splitting:
   - **Full overlap** → remove entire VMA.
   - **Partial overlap at edges** → truncate the VMA.
   - **Middle overlap** → split into two VMAs (left + right).
5. `flush_tlb()`.

### `do_mprotect(addr, len, prot)` — `vma.c:312`

1. Walks overlapping VMAs, updates `vm_flags` and `vm_page_prot`.
2. Iterates all 4KB PTEs in the range, updates permissions.
3. COW‑aware: if `PAGE_COW` is set, either allocates a private copy or promotes
   in‑place depending on `cow_count`.

---

## syscall interface

**Definitions** (`kernel/include/uapi/syscall.h`):

| Syscall | Number | Arguments |
|---------|--------|-----------|
| `SYS_mmap` | 44 | `rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset` |
| `SYS_mprotect` | 45 | `rdi=addr, rsi=len, rdx=prot` |
| `SYS_munmap` | 46 | `rdi=addr, rsi=len` |

The 6‑argument `mmap` uses a `syscall6()` wrapper (`libc/include/sys/syscall.h:102`)
that places args 4–6 in `r10`, `r8`, `r9` before `int $0x80`.

**Dispatch** in `do_system_call` (`trap.c:2020`):

```c
case SYS_mmap:      regs->rax = do_mmap(addr, length, prot, flags, fd, offset);
case SYS_mprotect:  regs->rax = do_mprotect(addr, length, prot);
case SYS_munmap:    regs->rax = do_munmap(addr, length);
```

---

## fork flow

```
do_fork()
  ├─ dup_task_struct()       — clone task_t, allocate stack
  ├─ fork_mm_copy()          — COW page table + VMA copy
  │   ├─ vmm_alloc_map()     — allocate child PML4
  │   ├─ walk parent PML4
  │   │   ├─ 2MB huge page   → eager copy (rep movsb)
  │   │   └─ 4KB PTE table   → iterate 512 PTEs, COW on writable
  │   ├─ fork_vma_copy()     — deep‑copy VMA list
  │   ├─ flush_tlb()         — parent's PTEs modified
  │   └─ child's cr3_out
  ├─ copy files / sighand
  ├─ set child state RUNNING
  └─ return child pid
```

### TLB flush after fork

`fork_mm_copy` modifies the **parent's** PTEs in‑memory (clears `R/W`, sets
`PAGE_COW`).  Only the current CPU runs the parent, so a local `flush_tlb()`
is sufficient.  Without this flush, the parent's TLB still holds stale
writable entries, which would bypass COW protection.

---

## Key files

| File | Role |
|------|------|
| `kernel/memory/pmm.c` | `subpage_pool`, `alloc_4k_page`, `free_4k_page`, `page_cow_get/put/refs` |
| `kernel/memory/vma.c` | `do_mmap`, `do_munmap`, `do_mprotect`, `vma_insert/remove/free_all`, `fork_vma_copy` |
| `kernel/memory/vmm.c` | `vmm_unmap_4k_page` (COW‑aware), `vmm_map_4k_page`, `vmm_pt_walk`, `vmm_alloc_map` |
| `kernel/sched/task.c` | `fork_mm_copy` (page table walk + COW install), `do_fork` |
| `kernel/arch/x86_64/trap.c` | `do_page_fault` COW resolution, `do_system_call` dispatch for `SYS_mmap/mprotect/munmap` |
| `kernel/include/kernel/vmm.h` | `PAGE_COW`, `PAGE_PROTNONE`, page table flag constants |
| `kernel/include/kernel/task.h` | `mm_t` (VMA list, `mmap_base`), `task_t` |
| `kernel/include/uapi/syscall.h` | `SYS_mmap` (44), `SYS_mprotect` (45), `SYS_munmap` (46) |
| `libc/unistd/mmap.c` | User‑space `mmap()`/`munmap()` wrappers using `syscall6`/`syscall` |
| `libc/unistd/mprotect.c` | User‑space `mprotect()` wrapper |
