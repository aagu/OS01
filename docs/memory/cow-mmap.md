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
#define PAGE_COW  (1UL << 10)   // kernel/include/arch/x86_64/pte.h
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

4. **Deep‑copy VMA list** via `fork_vma_copy()` (`arch/x86_64/memory/vma.c`).
5. **TLB flush** — parent's in‑memory PTEs were modified, a local flush is
   sufficient since only the current CPU runs the parent.

### COW fault resolution: `do_page_fault()` in `kernel/arch/x86_64/intr/trap.c:400`

When a user‑mode write hits a COW page (`#PF` with error code `0x03` = present +
write), the handler at `trap.c:449` checks `PAGE_COW`:

```c
if (pte && (*pte & PAGE_COW)) {
    uint64_t old_phys = *pte & PAGE_4K_MASK;
    if (page_cow_refs(old_phys) > 1) {
        // Multiple sharers: allocate new page, copy content, decrement count
        uint64_t new_phys = alloc_4k_page();
        memcpy(Phy_To_Virt(new_phys), Phy_To_Virt(old_phys), PAGE_4K_SIZE);
        *pte = x86_pte_4k_make(new_phys, x86_vma_pte_flags(vma->vm_flags));
        page_cow_put(old_phys);
    } else {
        // Last sharer: promote in-place
        page_cow_put(old_phys);
        *pte = x86_pte_4k_make(old_phys, x86_vma_pte_flags(vma->vm_flags));
    }
    flush_tlb();
}
```

- **`cow_count > 1`:** allocate a private 4KB page, copy old content, install
  the new page with full R/W permissions, decrement the old page's refcount.
- **`cow_count == 1`:** (this task is the sole remaining COW referencer)
  promote the existing page in‑place — set `R/W`, clear `PAGE_COW`, refcount→0.
  No allocation or copy needed.

### x86 COW-aware leaf teardown (`kernel/arch/x86_64/memory/vmm.c`)

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

### `do_mprotect` COW awareness (`kernel/arch/x86_64/memory/mmap.c`)

- **`PROT_NONE` stash:** preserves `PAGE_COW` when storing the physical address
  as `PROTNONE` — the COW reference is kept alive.
- **Restore from `PROTNONE`:** if `PAGE_COW` is set, re‑resolves COW just like
  the fault handler: copy if `cow_count > 1`, promote if `cow_count == 1`.

---

## VMA (Virtual Memory Area) subsystem

**Files:** `kernel/arch/x86_64/memory/mmap.c`, `kernel/include/sched/task.h` (`mm_t`)

### Data structures

```c
// kernel/include/sched/task.h:78
typedef struct mm_struct {
    uint64_t *pgdir;        // physical address of top-level page table (PGD/TTBR0)
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;      // sorted by vm_start
    uint64_t mmap_base;     // start search address for mmap
    spinlock_T lock;        // guards munmap/MAP_FIXED/mprotect/brk
} mm_t;

// kernel/arch/x86_64/memory/vma.c (vma_t, defined locally)
typedef struct vma {
    list_t      list;
    uint64_t    vm_start, vm_end;
    uint64_t    vm_flags;       // VMA_PROT_READ/WRITE/EXEC, VMA_SHARED/ANON...
                                    // (唯一权限来源：叶子位由 owning arch
                                    //  按需推导，x86 用 x86_vma_pte_flags)
    uint64_t    vm_pgoff;       // file offset in 4KB units
    vfs_node_t *vm_file;        // mapped file (NULL for anonymous)
} vma_t;
```

### Operations

| Function | Purpose |
|----------|---------|
| `vm_area_lookup` / `vm_area_insert_sorted` (`kernel/memory/vm_area.c`) | Shared sorted-list primitives; both ends' VMA edits call them directly. No merge (VMA count < 20). A zero-length VMA never matches an address lookup. |
| `vm_area_find_heap(mm)` | Shared heap finder: first `VMA_HEAP` node at `mm->start_brk`; zero-length allowed. |
| `x86_vma_destroy_node(vma)` | Destroy one VMA node: unlink + file-ref drop + node free. Does NOT free pages. Used by teardown, fork rollback and the retired-node drain. |
| `vma_free_all(mm)` | Withdraw every leaf in every VMA (borrowed `VMA_IO` leaves cleared, never released), then destroy all nodes. Called by exec/exit on retired/unpublished roots. |

### `do_mmap(addr, len, prot, flags, fd, offset)` — `arch/x86_64/memory/mmap.c`

1. **Validate** arguments (overflow, alignment, file existence for non‑anonymous).
2. **Find a hole** in the VMA space starting from `mm->mmap_base`.  Walks the
   sorted VMA list looking for a gap large enough.  For `MAP_FIXED`, calls
   `do_munmap` to clear the region first.
3. **Allocate VMA** and insert it into `vma_list`.
4. **Returns** the mapped address or `MAP_FAILED` (`-1`).

No physical pages are allocated at `mmap` time — they are demand‑paged on first
access via `do_page_fault` (anonymous: `alloc_4k_page`, file‑backed: `vfs_read`).

### `do_munmap(addr, len)` — `arch/x86_64/memory/mmap.c`

1. Aligns to 4KB boundaries.
2. Walks the VMA list, finds overlapping VMAs.
3. For each overlapped range: unmap all 4KB PTEs via `vmm_unmap_4k_page` (COW‑aware).
4. Handles VMA splitting:
   - **Full overlap** → remove entire VMA.
   - **Partial overlap at edges** → truncate the VMA.
   - **Middle overlap** → split into two VMAs (left + right).
5. `flush_tlb()`.

### `do_mprotect(addr, len, prot)` — `arch/x86_64/memory/mmap.c`

1. Walks overlapping VMAs, updates `vm_flags`（逻辑权限的唯一来源）。
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

## User Address Space & Heap Isolation (2026-10-02)

To isolate user heap allocations from the ELF image, prevent accidental over-mapping, and strictly govern permissions:

### 1. 4KB ELF Segment Loading and Envelope

- ELF segments are loaded strictly in 4KB pages via `elf_load_image()` in `kernel/arch/x86_64/fs/elf.c` (no 2MB huge PMD in user text/data).
- The user address space envelope is defined in `kernel/include/arch/x86_64/user_layout.h`: `X86_USER_MIN_ADDR = 0x400000`, heap capped at `X86_USER_HEAP_LIMIT = 0x203ff000`, the 2 MiB stack at `[X86_USER_STACK_BASE, X86_USER_STACK_END) = [0x20400000, 0x20600000)`.
- Validated at load time via pure validator `elf_layout_validate()` (`kernel/arch/x86_64/fs/elf.c`).

### 2. Heap VMA & Dynamic `brk` Management

- **Initialization**: `mm_init_user_heap(mm, elf_end)` creates a zero-length `VM_HEAP` VMA at page-aligned `elf_end` during exec/spawn.
- **Break management**: `mm_set_brk(mm, requested, &result)` (called by `SYS_brk` in `trap.c`):
  - **Expansion**: allocates and maps 4KB anonymous pages under `mm->lock`.
  - **Shrink**: unmaps and frees 4KB pages back to the subpage pool.
- **Fault guard**: In `do_page_fault`, uncommitted heap addresses (between `mm->end_brk` and `vma->vm_end`) cannot fault in writable pages.

### 3. Protected User Windows

`mm_user_range_protected(mm, start, end)` guards reserved user windows against `mmap` / `munmap` / `mprotect` collisions:
- ELF load envelope (`[0x400000, elf_end)`)
- Heap reserve (`[start_brk, heap_limit)`)
- Guard page (`[USER_STACK_BASE - PAGE_SIZE, USER_STACK_BASE)`)
- User stack (`[USER_STACK_BASE, USER_STACK_TOP)`)

---

## AArch64 COW / 匿名 VMA（2026-11-11 ~ 2026-11-13 闭环）

> 本节描述 aarch64 侧的模型。上文的 COW 叙述是 x86_64 的 PTE-flag 语义；
> aarch64 **不把 COW 编码进页表**，两者的所有权表达不同。

### 1. 所有权模型：驻留记录 = COW 所有权，PTE = 硬件权限

aarch64 的用户页由**驻留记录**（resident record）拥有，记录携带来源、逻辑权限与
独占/共享状态；ELF 与栈装载器改走 `user_vm_map_image_page()`，不再自持 VA/PA 清单。

- **逻辑权限与硬件权限分离**：`PROT_NONE` 与共享页在硬件上保持 RO/NX，逻辑权限随 VMA 提交。
  共享只读页与 stash（驻留但未实例化）共用同一引用契约，因此 `mprotect` 恢复写权限时
  仍能判定是否仍需复制。
- **引用计数在 PMM 一侧**：`pmm_cow_acquire()` / `pmm_cow_release()` 负责分配位、槽位、
  模式与 `UINT16_MAX` 检查；驻留销毁时**丢弃共享引用**而不是释放仍被拥有的页。
- **换 PA 走受检入口**：aarch64 侧 `aarch64_user_vm_replace_cow()` 是 M4 专用的 COW
  替换后端入口（受管根写闸、RO/NX 旧叶、BBM 换 PA、旧 PA 所有权保留）。

### 2. 匿名 VMA 与按需缺页

aarch64 的匿名 VM 策略在 `kernel/arch/aarch64/memory/user_vm_policy.c`
（策略头 `kernel/include/arch/aarch64/user_vm_policy.h`），叶适配层在
`kernel/arch/aarch64/memory/user_vm_leaf.c`（`arch_user_vm_*`，声明在
`kernel/include/arch/aarch64/user_vm_leaf.h`），fork 事务在
`kernel/arch/aarch64/memory/user_vm_fork.c`（与策略共享
`kernel/include/arch/aarch64/user_vm_internal.h` 的内部访问器），
共享 VMA 原语在 `kernel/memory/vm_area.c`。x86 的用户 VM 按生命周期拆分：
`brk.c`（堆 VMA + brk 提交/回滚）、`mmap.c`（mmap/munmap/mprotect 事务）、
`vma.c`（节点销毁 + fork 元数据 + 退休根清理）、`uaccess.c`（内核→用户
写入预备对），声明统一在 `kernel/include/arch/x86_64/vma.h`。两个 VM
策略不合并，`mm_t` 布局不变。

- **只提交元数据**：`mmap`/`brk` 返回后目标页仍无叶子；首次合法访问才分配清零的 4 KiB 页。
- **切离编辑**：修改 VMA/叶前先切离用户根（撤销 root）、IRQ 屏蔽下运行策略、再恢复原根与
  TLS；活动根上的写操作一律拒绝（沿用 M3 契约），TLB 完成点固定在切离与重装两个边界。
- **stash**：`PROT_NONE` 与解映射保留驻留记录，页可被释放后按需重新实例化。
- **stash**：`PROT_NONE` 与解映射保留驻留记录，页可被释放后按需重新实例化。
- **VMA 只表达语义**（2026-11-14 边界清理）：公共 `vma_t` 只有逻辑权限位
  （`VMA_PROT_*`），不再保存硬件 PTE 字（`vm_page_prot` 已删除）。叶子位由拥有
  架构按需推导（x86：`x86_vma_pte_flags(vm_flags)`，USER/PRESENT/NX/WRITE）；
  mprotect 在原位叠加旧叶的缓存位与 COW 状态，转换本身不触碰缓存/COW。
- **设备回调跨边界不携带描述符**：`/dev/fb` mmap 链路里，驱动 → VFS → MM 的提交
  回调 `vfs_mmap_commit_fn` 只传首个页的物理地址加语义缓存请求
  （`enum vm_device_cache`，UC/WB，见 `<memory/vm_device.h>`）；x86 提交端先校验
  枚举（未知值 `-EINVAL`，无任何副作用），再用 VMA 逻辑位 + 请求的内存类推导
  PCD/PWT 与 USER/PRESENT/NX/WRITE 叶子（编码统一在 `<arch/x86_64/fb_pte.h>`，
  内核映射 / `/dev/fb` 用户别名 / 早期别名三者不漂移）。设备 VMA 仍是无文件的
  `VMA_IO`，缓存真值由已提交叶子和显示后端持有。

### 3. fork 与写时复制

- **clone 事务**：`user_vm_clone_fork()` 复制布局与 VMA，把每个驻留页借给子根，逐页提交
  一次受检 PMM COW 引用（私有 0→2、共享 +1），把父的可写叶降为 RO/NX，**最后一步才发布
  子任务**；任一步失败精确回滚已完成步骤。**child PA 从不被释放** —— 回滚的是引用。
- **写故障路径**：EL0 故障分类器把翻译故障与 level-3 写权限故障（FSC `0x0f`、WnR、
  未置 CM/S1PTW）分开，后者走 `user_vm_cow_fault()`：多引用共享页经受检替换入口复制并丢弃
  旧引用，唯一引用就地升级并释放其计数。
- **`brk` 收缩**在清零或释放任何页之前先私有化保留尾页，OOM 时恢复原状。
- **`prepare_copy`** 接受逻辑 RW 的共享 COW 目标，在触碰任何 COW 叶或计数前预分配私有副本与
  临时空叶，失败时双向回滚源。

### 4. 边界

验收范围是**单核 BSP 自测启动**（`AARCH64_EL0_TEST=1`，`SMP=1`）。未覆盖：VFS `/bin/init`、
waitpid、信号、文件映射、`MAP_FIXED`、共享映射、设备映射、SMP 用户调度、IRQ 返回抢占。
规划项见 [roadmap.md](../roadmap.md) P2；现状叙述见 [arch.md](../arch.md#aarch64-用户态现状m42026-11-11--2026-11-13-闭环)。

---

## Key files

| File | Role |
|------|------|
| `kernel/memory/pmm.c` | `subpage_pool`, `alloc_4k_page`, `free_4k_page`, `page_cow_get/put/refs`, `pmm_cow_acquire/release` |
| `kernel/arch/x86_64/memory/vma.c` | x86 VMA 节点销毁（`x86_vma_destroy_node`）、fork 元数据复制（`fork_vma_copy`）、退休/未安装根清理（`vma_free_all`） |
| `kernel/arch/x86_64/memory/brk.c` | `mm_init_user_heap`、`mm_set_brk`（成功安装 VA 日志；失败两轮撤销：就地取旧描述符 → 本地同步 → 释放） |
| `kernel/arch/x86_64/memory/mmap.c` | `do_mmap`, `do_munmap`, `do_mprotect`, `mm_user_range_protected` 及其预检/分裂/文件偏移 helper |
| `kernel/arch/x86_64/memory/vmm.c` | x86 raw 页表 API + COW-aware teardown + 双 walker（`arch_virt_to_phys` / `arch_user_range_accessible`） |
| `kernel/arch/aarch64/memory/user_vm_policy.c` | aarch64 匿名 VM 策略：按需缺页、stash、`user_vm_clone_fork`、`user_vm_cow_fault`、`prepare_copy` |
| `kernel/memory/vm_area.c` | 双架构共享的 VMA 列表原语 |
| `kernel/memory/mm.c` | 双架构共享的 `mm_alloc` |
| `kernel/arch/aarch64/memory/user_vm.c` | aarch64 事务层：授权、IRQ/TLS 登记、根切离与恢复 |
| `kernel/arch/aarch64/memory/user_vm_leaf.c` | aarch64 叶适配层（`arch_user_vm_*` 三态适配） |
| `kernel/arch/aarch64/memory/user_vm_fork.c` | aarch64 fork 事务（clone 阶段与精确回滚） |
| `kernel/arch/aarch64/memory/user_mm.c` | `aarch64_user_mm_create` / `aarch64_user_mm_destroy`：mm 拥有的私有用户根 |
| `kernel/arch/aarch64/memory/vmm_backend.c` | `aarch64_user_vm_replace_cow`：受检 COW 换 PA 后端入口 |
| `kernel/arch/x86_64/fs/elf.c` | 4KB per-page ELF image loader and `elf_layout_validate` (x86-only) |
| `kernel/sched/task.c` | `fork_mm_copy` (page table walk + COW install), `do_fork`, staged spawn/exec image lifecycle |
| `kernel/arch/x86_64/intr/trap.c` | `do_page_fault` COW resolution and heap guard, `do_system_call` dispatch for `SYS_mmap/mprotect/munmap/brk` |
| `kernel/include/arch/x86_64/pte.h` | `PAGE_COW`, `PAGE_PROTNONE`, x86 PTE 描述符位 |
| `kernel/include/memory/vma.h` | 公共 `vma_t`、`VMA_*` 逻辑标志、`MAP_`/`PROT_*` 常量（无操作声明） |
| `kernel/include/arch/x86_64/vma.h` | x86 VMA 事务层声明：`do_mmap/mprotect/munmap`、heap/保护区 helpers、内联 `x86_vma_pte_flags` |
| `kernel/include/memory/vmm.h` | 仅 `tlb_shootdown(void)`（共享 TLB 广播协议） |
| `kernel/include/arch/x86_64/vmm.h` / `arch/x86_64/mmu.h` | x86 raw 页表 API、`ptbl_t/kernel_map`、根与单页原语、walker 声明 |
| `kernel/include/memory/uaccess.h` | 公共容错拷贝 `copy_*_ft(_res)` 与 `syscall_check_user_range` |
| `kernel/include/arch/x86_64/uaccess.h` | x86 `os01_jmp_buf`、`strnlen_user`、写入预备对 |
| `kernel/include/arch/x86_64/user_layout.h` | x86 用户布局常量（`X86_USER_MIN_ADDR`、栈窗口、`X86_USER_HEAP_LIMIT`） |
| `kernel/include/arch/aarch64/user_vm_policy.h` | 匿名 VM 策略接口（按需缺页 / clone / COW 故障 / copy 预备） |
| `kernel/include/arch/aarch64/user_vm_leaf.h` | aarch64 叶适配器声明（`arch_user_vm_*` 三态） |
| `kernel/include/arch/aarch64/user_vm.h` | aarch64 用户 VM 事务层：授权、切离、恢复 |
| `kernel/include/arch/aarch64/user_mm.h` | `aarch64_user_mm_create` / `aarch64_user_mm_destroy` 与根归属契约 |
| `kernel/include/sched/task.h` | `mm_t` (VMA list, `mmap_base`, `pgdir`, `lock`), `task_t` |
| `kernel/include/uapi/syscall.h` | `SYS_mmap` (44), `SYS_mprotect` (45), `SYS_munmap` (46), `SYS_brk` (12) |
| `libc/unistd/mmap.c` | User‑space `mmap()`/`munmap()` wrappers using `syscall6`/`syscall` |
| `libc/unistd/mprotect.c` | User‑space `mprotect()` wrapper |
| `libc/unistd/brk.c` | User‑space `brk()`/`sbrk()` wrappers |
