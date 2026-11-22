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
文件映射、`MAP_FIXED`、共享映射、设备映射、SMP 用户调度、IRQ 返回抢占。
**waitpid / exec / 信号三块已于 2026-11-14 闭环（M4.12 ~ M4.14）**，不再是未覆盖项。
规划项见 [roadmap.md](../roadmap.md) P2；现状叙述见 [arch.md](../arch.md#aarch64-用户态现状m42026-11-11--2026-11-18-闭环)。

---

## x86 NX 按 VMA 策略强制执行（2026-11-13）

> commit `cce539d1`「feat(mm): enforce x86 PTE execute permissions per VMA policy」，
> 配套 spec [2026-11-13-x86-pte-permissions-design.md](../superpowers/specs/2026-11-13-x86-pte-permissions-design.md)。
> 闭环记录见 `docs/changelog.md` 2026-11-13 条目。

**原则**：叶权限的**唯一来源**是 VMA 的逻辑权限位，硬件 PTE 位不写进 VMA，而是由拥有者架构
在需要装叶时按需换算（`kernel/include/arch/x86_64/vma.h`）：

```c
static inline uint64_t x86_vma_pte_flags(uint64_t vm_flags)
{
    uint64_t f = PAGE_USER;
    if (vm_flags & VMA_PROT_READ)  f |= PAGE_VALID;
    if (vm_flags & VMA_PROT_WRITE) f |= PAGE_WRITE;
    if (!(vm_flags & VMA_PROT_EXEC)) f |= PAGE_NO_EXEC;   /* ← NX */
    return f;
}
```

`PAGE_NO_EXEC` 是 x86 PTE bit 63（`kernel/include/arch/x86_64/pte.h:20`）。

**覆盖面**：

| 区域 | 权限 | 位置 |
|------|------|------|
| 用户栈（两条 exec 路径） | `PAGE_USER_PMD \| PAGE_NO_EXEC` | `kernel/arch/x86_64/sched/exec.c:526`、`:716` |
| 用户堆 | VMA 为 `R\|W\|ANON\|HEAP`，**无 EXEC** ⇒ 叶子 RW/NX | `kernel/arch/x86_64/memory/brk.c:61` |
| `mmap` / `mprotect` | `prot_to_vma_flags()` 把 `PROT_*` 映射成逻辑位（`PROT_EXEC` 隐含 READ），叶子按 VMA 换算 | `kernel/arch/x86_64/memory/mmap.c:73-97`、`:772` |
| 设备映射 | 设备驱动不授予 EXEC ⇒ 叶子带 NX | `kernel/arch/x86_64/memory/mmap.c:422-426` |
| ELF 分段叶子（2026-11-20 起） | W 段（`.data`/`.bss`）RW+NX；RO 非 X 段（`.rodata`）RO+NX；仅 `PF_X` 段（`.text`）可执行；显式 `PF_W\|PF_X` 段保留 RWX（与 VMA 的 RWX 放宽对齐） | `kernel/arch/x86_64/fs/elf.c`（`elf_pte_flags()` / `pte_flags_compatible()`） |
| 硬件 | 启动期打开 `EFER.NXE` | `kernel/arch/x86_64/platform/boot.c:115-117` |

**故障期强制**：首次触碰就按 VMA 校验——未授权写、PROT_NONE 访问、未授权**取指**
分别以 `SIGSEGV` 终止当前用户任务（`kernel/arch/x86_64/intr/trap.c:742-763`）；
后续按需填充与 COW 解析路径一律用 `x86_vma_pte_flags(vma->vm_flags)` 装叶，
因此逻辑权限始终是权威。

**运行期证据**：systest 用例 `mmap_exec_protection`（RW 段取指 → SIGSEGV；RX 段写 → SIGSEGV；
RX/RWX 段可执行；文件映射 RX 首写 → SIGSEGV；RW 段取指 → SIGSEGV；真实 2 MiB 栈叶不可执行）。
宿主断言见 `hosttests/cases/test_process_image_lifecycle.c`（堆叶 `PAGE_USER|PAGE_WRITE|PAGE_VALID|PAGE_NO_EXEC`）。

### ELF 分段叶子 NX 细化（2026-11-20）

VMA 策略落地时 ELF 装载器仍按 Task 2 简报「不细化分段 NX」直接写叶（`.data`/`.bss` 为 RWX、
`.rodata` 为 ROX）。装载器不建 VMA（预映射整像、无按需缺页），故上表策略覆盖不到 ELF 叶，
只能装载器自身按 `p_flags` 换算（`kernel/arch/x86_64/fs/elf.c`）：

```c
static inline uint64_t elf_pte_flags(uint32_t p_flags)
{
    uint64_t f = (p_flags & PF_W) ? PAGE_USER_PTE : PAGE_USER_PTE_RO;
    if (!(p_flags & PF_X)) f |= PAGE_NO_EXEC;   /* ← 与 x86_vma_pte_flags 对齐 */
    return f;
}
```

- **段级**：W 段 RW+NX（`.data`/`.bss` 不再 RWX）；R 无 X 段 RO+NX（`.rodata` 不再 ROX）；
  仅 `PF_X` 段可执行（`.text`）；显式 `PF_W|PF_X` 段保留 RWX——与 VMA 路线对 RWX 的放宽一致。
- **共享页合并**（两 PT_LOAD 字节不相交但同处一个 4 KiB 叶）：`pte_flags_compatible()` 从
  只查 W 位扩为 **W/X 双轴超集检查**（X = 无 `PAGE_NO_EXEC`）。先映射者定型，永不改写叶标志；
  可写段与可执行段共享同页时两请求必然冲突 → `-ENOEXEC`（共享页要么保持 W^X 干净，要么 exec
  前置失败，绝不静默变 RWX）。读回既有 PTE 标志时须把 `PAGE_NO_EXEC`（bit 63）与低 12 位一起
  掩出。
- **NX 的全生命周期保持**：fork 的 COW 降级整字复制 PTE（清 W、置 COW，NX 不动）；
  fork 后写私有化走 trap.c **无 VMA COW 分支**（`x86_pte_4k_flags(*upte) | PAGE_WRITE`，
  NX 随原叶保留）；mm 销毁按 PTE 存在性回收，与 NX 无关。
- **入口约束**：`elf_layout_validate` 要求 entry 落在可执行 PT_LOAD 内，与「仅 .text 可执行」自洽。

**验收**：`hosttests/cases/test_elf_load_4k.c`（24 用例）：W 叶=RW+NX、BSS 叶=RW+NX、
`.rodata` 叶=RO+NX、`.text` 叶=R+X、显式 RWX 段保留可执行、NX 共享页 + 可执行请求 →
`-ENOEXEC`（回滚恰好一次）、RO 共享页并入 RX 页保留 X、RO 并入 RW 页保留 W+NX；
宿主桩 `elf_load_stubs.c` 的 PTE 存储字同步补 `PAGE_NO_EXEC` 保留（镜像
`x86_pte_4k_make`，否则装载器读不回共享页 NX）。`make test-host` EXIT=0
（239 binaries）；`make test-static` PASS；`make test-qemu SUITE=phase-0`
（产品镜像 boot → ash 提示符）与 `make OS01_SYSTEST=1 test-syscall`
（123 项 syscall E2E，含既有 `mmap_exec_protection` NX 用例）全部 PASS。

---

## x86_64 `mmap_base` ASLR (ASLR-A)

On x86_64 user address spaces (`spawn_user_task()` and `sys_exec()`), `mm->mmap_base` is randomized per image build via `x86_aslr_init_mmap_base()` (`kernel/arch/x86_64/memory/aslr.c`):

- **Window & entropy width (`kernel/include/arch/x86_64/user_layout.h`)**: 28-bit page-granular window `[X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END)` = `[0x0000400000000000, 0x0000410000000000)` (64 TiB..65 TiB, 1 TiB span, `2^28` 4 KiB slots).
- **STRONG-only fail-closed**: `x86_aslr_init_mmap_base()` calls `kernel_random_get_strong()` independently of `AT_RANDOM` and wipes the 32-byte stack buffer on both success and failure paths. When STRONG entropy is unavailable, it preserves `mm->mmap_base` and returns `-EAGAIN` (no silent fallback to `0x40000000` or WEAK entropy).
- **`fork()` inheritance**: `mm_alloc()` keeps the deterministic `0x40000000` default for dual-arch parity; `fork_mm_copy()` inherits `parent_mm->mmap_base` without consuming entropy.

## x86_64 Static PIE ASLR (ASLR-B)

- **Zero-linked static PIE**: Default x86_64 userland ELFs and BusyBox are linked at base `0` with `libc/arch/x86_64/user-pie.ld` (`ET_DYN` + `DF_1_PIE`, `R_X86_64_RELATIVE`-only relocations, `-z text -z norelro --pack-dyn-relocs=none`). `ET_EXEC` compatibility is preserved for standalone fixtures (`aslr_b_exec.S`) and `aarch64` remains `ET_EXEC`.
- **Two-phase validation & per-page relocation**: `x86_elf_pie_prepare()` (`kernel/arch/x86_64/fs/elf_pie.c`) validates the `ET_DYN` header, `PT_LOAD` geometry (`<= X86_USER_PIE_LINK_END = 0x02000000`), forbidden headers (`PT_INTERP`/`PT_TLS`/`PT_GNU_RELRO`/exec `PT_GNU_STACK`), `PT_DYNAMIC` allowlist, and `R_X86_64_RELATIVE` records before any page allocation. `elf_load()` draws `pie_bias = 0x00400000 + ((raw & 0xffff) << 12)` in `[0x00400000, 0x10400000)` via `x86_aslr_get_pie_bias()`, maps each 4 KiB page directly, and applies cross-page `R_X86_64_RELATIVE` relocations per page without extra writable copies.
- **3-stage RNG & auxv ABI**: PIE bias, `mmap_base`, and `AT_RANDOM` each call `kernel_random_get_strong()` independently and fail closed with `-EAGAIN` on entropy failure without leaking pages or mutating the caller's `mm` on `sys_exec` failure. `setup_user_stack()` emits the 9-pair PIE auxv (`AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_BASE=0`, `AT_PAGESZ`, `AT_PLATFORM`, `AT_RANDOM`, `AT_NULL`).
- **Window, gap-search & ceiling (`kernel/arch/x86_64/memory/mmap.c`)**:
  - `mmap_effective_limit(addr_limit)` clamps `addr_limit & PAGE_4K_MASK` to `X86_USER_MMAP_MAX_ADDR` (`0x00007FFFFFFFF000ULL`), reserving the top canonical guard page `[0x00007FFFFFFFF000, 0x0000800000000000)` and rejecting non-canonical ranges (`do_mmap` returns `-ENOMEM`, `do_munmap`/`do_mprotect` return `-EINVAL`).
  - `MAP_FIXED` at `addr == 0` (or below `X86_USER_MIN_ADDR`) is rejected with `-EINVAL`.
  - `mmap_find_gap_locked(mm, length, upper_limit, &out_addr)` performs overflow-safe, non-wrapping first-fit scanning within `[max(mmap_base, X86_USER_STACK_END), upper_limit)`, unconditionally advancing past any VMA whose preceding gap is smaller than `length` and returning `-ENOMEM` without wrapping below `mmap_base`.

---

## Key files

| File | Role |
|------|------|
| `kernel/memory/pmm.c` | `subpage_pool`, `alloc_4k_page`, `free_4k_page`, `page_cow_get/put/refs`, `pmm_cow_acquire/release` |
| `kernel/arch/x86_64/memory/vma.c` | x86 VMA 节点销毁（`x86_vma_destroy_node`）、fork 元数据复制（`fork_vma_copy`）、退休/未安装根清理（`vma_free_all`） |
| `kernel/arch/x86_64/memory/brk.c` | `mm_init_user_heap`、`mm_set_brk`（成功安装 VA 日志；失败两轮撤销：就地取旧描述符 → 本地同步 → 释放） |
| `kernel/arch/x86_64/memory/mmap.c` | `do_mmap`, `do_munmap`, `do_mprotect`, `mmap_effective_limit`, `mmap_find_gap_locked`, `mm_user_range_protected` |
| `kernel/arch/x86_64/memory/aslr.c` | `x86_aslr_derive_mmap_base`, `x86_aslr_init_mmap_base`, `x86_aslr_get_pie_bias` (x86_64 STRONG-only fail-closed ASLR) |
| `kernel/arch/x86_64/memory/vmm.c` | x86 raw 页表 API + COW-aware teardown + 双 walker（`arch_virt_to_phys` / `arch_user_range_accessible`） |
| `kernel/arch/aarch64/memory/user_vm_policy.c` | aarch64 匿名 VM 策略：按需缺页、stash、`user_vm_clone_fork`、`user_vm_cow_fault`、`prepare_copy` |
| `kernel/memory/vm_area.c` | 双架构共享的 VMA 列表原语 |
| `kernel/memory/mm.c` | 双架构共享的 `mm_alloc` |
| `kernel/arch/aarch64/memory/user_vm.c` | aarch64 事务层：授权、IRQ/TLS 登记、根切离与恢复 |
| `kernel/arch/aarch64/memory/user_vm_leaf.c` | aarch64 叶适配层（`arch_user_vm_*` 三态适配） |
| `kernel/arch/aarch64/memory/user_vm_fork.c` | aarch64 fork 事务（clone 阶段与精确回滚） |
| `kernel/arch/aarch64/memory/user_mm.c` | `aarch64_user_mm_create` / `aarch64_user_mm_destroy`：mm 拥有的私有用户根 |
| `kernel/arch/aarch64/memory/vmm_backend.c` | `aarch64_user_vm_replace_cow`：受检 COW 换 PA 后端入口 |
| `kernel/arch/x86_64/fs/elf.c` | 4KB per-page ELF image loader (`ET_EXEC` + `ET_DYN` static PIE) and `elf_layout_validate` (x86-only) |
| `kernel/arch/x86_64/fs/elf_pie.c` | `x86_elf_pie_prepare`, `x86_elf_pie_dispose`, `R_X86_64_RELATIVE` relocation validator |
| `kernel/sched/task.c` | `fork_mm_copy` (page table walk + COW install), `do_fork`, staged spawn/exec image lifecycle |
| `kernel/arch/x86_64/intr/trap.c` | `do_page_fault` COW resolution and heap guard, `do_system_call` dispatch for `SYS_mmap/mprotect/munmap/brk` |
| `kernel/include/arch/x86_64/pte.h` | `PAGE_COW`, `PAGE_PROTNONE`, x86 PTE 描述符位 |
| `kernel/include/memory/vma.h` | 公共 `vma_t`、`VMA_*` 逻辑标志、`MAP_`/`PROT_*` 常量（无操作声明） |
| `kernel/include/arch/x86_64/vma.h` | x86 VMA 事务层声明：`do_mmap/mprotect/munmap`、heap/保护区 helpers、内联 `x86_vma_pte_flags` |
| `kernel/include/memory/vmm.h` | 仅 `tlb_shootdown(void)`（共享 TLB 广播协议） |
| `kernel/include/arch/x86_64/vmm.h` / `arch/x86_64/mmu.h` | x86 raw 页表 API、`ptbl_t/kernel_map`、根与单页原语、walker 声明 |
| `kernel/include/memory/uaccess.h` | 公共容错拷贝 `copy_*_ft(_res)` 与 `syscall_check_user_range` |
| `kernel/include/arch/x86_64/uaccess.h` | x86 `os01_jmp_buf`、`strnlen_user`、写入预备对 |
| `kernel/include/arch/x86_64/user_layout.h` | x86 用户布局常量（`X86_USER_MIN_ADDR`、栈窗口、`X86_USER_HEAP_LIMIT`、`X86_USER_ADDR_LIMIT`、`X86_USER_MMAP_MAX_ADDR`、`X86_USER_MMAP_RND_*`） |
| `kernel/include/arch/x86_64/aslr.h` | x86_64 ASLR 私有接口声明（`x86_aslr_derive_mmap_base`、`x86_aslr_init_mmap_base`） |
| `kernel/include/arch/aarch64/user_vm_policy.h` | 匿名 VM 策略接口（按需缺页 / clone / COW 故障 / copy 预备） |
| `kernel/include/arch/aarch64/user_vm_leaf.h` | aarch64 叶适配器声明（`arch_user_vm_*` 三态） |
| `kernel/include/arch/aarch64/user_vm.h` | aarch64 用户 VM 事务层：授权、切离、恢复 |
| `kernel/include/arch/aarch64/user_mm.h` | `aarch64_user_mm_create` / `aarch64_user_mm_destroy` 与根归属契约 |
| `kernel/include/sched/task.h` | `mm_t` (VMA list, `mmap_base`, `pgdir`, `lock`), `task_t` |
| `kernel/include/uapi/syscall.h` | `SYS_mmap` (44), `SYS_mprotect` (45), `SYS_munmap` (46), `SYS_brk` (12) |
| `libc/unistd/mmap.c` | User‑space `mmap()`/`munmap()` wrappers using `syscall6`/`syscall` |
| `libc/unistd/mprotect.c` | User‑space `mprotect()` wrapper |
| `libc/unistd/brk.c` | User‑space `brk()`/`sbrk()` wrappers |
