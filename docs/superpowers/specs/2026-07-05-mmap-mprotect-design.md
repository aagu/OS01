# mmap/mprotect syscall 实现设计

> **日期**: 2026-07-05
> **状态**: 待实现
> **依赖**: 4KB PTE 页面支持
> **后续**: COW fork（本设计完成其前置条件）

---

## 目标

实现 `mmap`、`mprotect`、`munmap` 三个 syscall，支持匿名映射和只读文件映射，为 COW fork 和 busybox 核心 applet 提供内存管理基础。

## 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 物理页面粒度 | 4KB PTE 层 | x86_64 硬件原生支持，COW fork 前置条件 |
| 2 | VMA 分配对齐 | 4KB 对齐（与 Linux 一致） | 16KB 簇作为可选内部策略，不强制；VMA vm_start/end 对齐到 4KB |
| 3 | syscall ABI | `syscall6()` 宏（r10/r8/r9 传参） | 扩展 libc 接口而非内核直接读 pt_regs，保持关注分离 |
| 4 | VMA 组织 | 单向链表按 `vm_start` 排序 | 当前 busybox 场景 VMA < 20，O(n) 足够；接口预留后续替换为红黑树 |
| 5 | 文件映射范围 | 仅 `MAP_PRIVATE` 只读 | busybox grep/sed 实际需求；回写和共享映射推迟 |
| 6 | mprotect 范围 | `PROT_NONE` / `PROT_READ|PROT_WRITE` | 先覆盖核心权限变更，`PROT_EXEC` 后续 |

## 架构

```
用户态                       内核态
------                      ------
mmap(addr,len,prot,         syscall6(SYS_mmap, ...)
     flags,fd,off)            → do_mmap()
                                ├─ 参数校验 + flags 翻译
                                ├─ VMA 分配 + 插入 mm_t.vma_list
                                ├─ 匿名：不立即分配物理页
                                ├─ 文件：不立即读文件
                                └─ 返回虚拟地址

mprotect(addr,len,prot)     syscall(SYS_mprotect, ...)
                              → do_mprotect()
                                └─ 遍历 VMA → 更新 PTE 权限位

munmap(addr,len)            syscall(SYS_munmap, ...)
                              → do_munmap()
                                └─ 遍历 VMA → 释放物理页 + PTE → 移除 VMA

#PF (缺页异常)              do_page_fault()
                              ├─ vma_find(cr2) → 无匹配 → SIGSEGV
                              ├─ Write fault → 权限检查
                              ├─ 匿名首次访问 → 分配 4KB 页 → 填充 PTE
                              └─ 文件首次访问 → 分配 4KB 页 → vfs_read → 填充 PTE
```

## 组件

### 1. VMA 数据结构 (`kernel/include/kernel/vma.h`)

```c
#define VM_READ     0x01
#define VM_WRITE    0x02
#define VM_EXEC     0x04
#define VM_SHARED   0x08
#define VM_MAYSHARE 0x10
#define VM_ANON     0x20   // 匿名映射（无文件关联）
#define VM_GROWSDOWN 0x40  // 预留，不实现

typedef struct vm_area_struct {
    list_t list;
    uint64_t vm_start;    // 起始 VA（16KB 对齐）
    uint64_t vm_end;      // 结束 VA（16KB 对齐）
    uint64_t vm_flags;    // VM_*
    uint64_t vm_page_prot;// 对应 PAGE_* flags
    uint64_t vm_pgoff;    // 文件偏移（4KB 页号）

    struct vfs_node *vm_file;  // NULL = 匿名映射
} vma_t;
```

`mm_t` 新增字段：
```c
list_t vma_list;    // vma_t 链表，按 vm_start 升序
uint64_t mmap_base;  // mmap 下次分配的起始搜索地址
```

### 2. 4KB PTE 层 (`kernel/memory/vmm.c` / `kernel/include/kernel/vmm.h`)

`PAGE_4K_SHIFT`/`PAGE_4K_SIZE`/`PAGE_4K_MASK` 已在 `pmm.h` 定义，无需重复。

新增 PTE 标志宏（不含 `PAGE_PS`，硬件识别为 4KB 页）：
```c
#define PAGE_USER_4K     (PAGE_U_S | PAGE_R_W | PAGE_Present)  // 用户 R/W 4KB
#define PAGE_USER_4K_RO  (PAGE_U_S | PAGE_Present)             // 用户只读 4KB
#define PAGE_KERNEL_4K   (PAGE_R_W | PAGE_Present)             // 内核 4KB
```

新增函数（`vmm.c` / `vmm.h`）：

- **`vmm_map_4k_page(pagemap, phys, virt, flags)`** — 在 PML2[level2] 检查/分配 PTE 表（若不存在则 `calloc` 一个 4KB 页写入 PML2 条目），在 PTE[level1] 处写入 `phys | flags`（不含 `PAGE_PS`）。
- **`vmm_unmap_4k_page(pagemap, virt)`** — 清除 PTE[level1] 条目 + `free_pages` 释放物理页。若 PTE 表全空则 `kfree` 回收 PTE 表 + 清零 PML2 条目。
- **`vmm_pt_walk(pagemap, virt, allocate)`** — 返回 `uint64_t *` 指向 PTE 条目。若 `allocate=true` 且 PTE 表不存在则自动分配。被 `vmm_map_4k_page` 和 `vmm_unmap_4k_page` 共用。

### 3. syscall6() 宏 (`libc/include/sys/syscall.h`)

```c
static inline int64_t syscall6(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8  __asm__("r8")  = arg5;
    register uint64_t r9  __asm__("r9")  = arg6;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3),
          "r" (r10), "r" (r8), "r" (r9)
        : "memory");
    return ret;
}
```

`int $0x80` 入口已保存完整 `pt_regs_t`（包括 r8/r9/r10），内核侧无需改动 entry.S。

### 4. do_mmap (`kernel/memory/vma.c`)

```
do_mmap(addr, length, prot, flags, fd, offset):
  1. 参数校验
     - length == 0 → -EINVAL
     - 未知 prot 位 → -EINVAL
     - PROT_WRITE 无 PROT_READ → -EINVAL（x86 硬件不支持只写页）
     - MAP_ANONYMOUS 必须有 fd == -1 → -EINVAL
     - 非 MAP_ANONYMOUS 必须 fd 有效且 fd 对应 vfs_node 非 NULL → -EBADF
     - MAP_FIXED 且 addr 非页对齐 → -EINVAL
     - MAP_FIXED 且 addr >= current->addr_limit → -ENOMEM
  
  2. 地址计算
     - 若 MAP_FIXED：使用 addr（4KB 对齐）
       重叠 VMA 先 munmap（调用 do_munmap 清理 [addr, addr+length) 内的现存 VMA）
     - 否则：从 mm->mmap_base 开始向上搜索空闲区间
     - 长度对齐到 4KB 上界（16KB 簇仅影响 VMA 合并策略，不对齐强制）

  3. flags 翻译
     prot → vm_page_prot:
       PROT_READ        → PAGE_USER_4K_RO
       PROT_READ|WRITE  → PAGE_USER_4K
       PROT_EXEC        → PAGE_USER_4K（暂不设 NX，与现有 ELF PAGE_USER_Page 一致）
       PROT_NONE        → PAGE_U_S（无 Present + 无 R/W，VMA 的 vm_flags 全 0）
     vm_flags: MAP_SHARED → VM_SHARED, MAP_PRIVATE → 0
                MAP_ANONYMOUS → VM_ANON
  
  4. 文件映射：vfs_node_get(vma->vm_file) 增加引用计数
     限制：若 vm_file 所在 fs 非内存型（如 AHCI-backed FAT），返回 -ENODEV
     （第一版 do_page_fault 在 IST 栈上不可调度，仅支持内存 VFS）
  
  5. 分配 vma_t（kmalloc）
     填充字段，插入 mm->vma_list（按 vm_start 升序）
     **不合并**相邻 VMA — 减少 insert 复杂度和 bug 面（<20 VMA 场景收益可忽略）

  6. 若返回地址 >= current->addr_limit → -ENOMEM
     返回 vm_start
```

### 5. do_mprotect

```
do_mprotect(addr, length, prot):
  1. addr 4KB 对齐，length 上界对齐
  2. 遍历 addr..addr+length 覆盖的 VMA
  3. 对每个 VMA：
     - 更新 vm_page_prot 和 vm_flags（同步 VM_READ/VM_WRITE/VM_EXEC）
       PROT_NONE → vm_flags 清除 VM_READ|VM_WRITE|VM_EXEC
       PROT_READ  → vm_flags |= VM_READ，vm_page_prot = PAGE_USER_4K_RO
       以此类推
     - 对已有物理页：遍历 PTE 条目，更新权限位
       PROT_NONE → 清 PAGE_Present + PAGE_R_W（保留物理页）
     - 若有修改 PTE → flush_tlb()（单任务用户页表修改无需 IPI broadcast，
       当前 tlb_shootdown 对 user_pml4 ≠ kernel_map 是 no-op，但调用无害）
  4. 返回 0
```

### 6. do_munmap

```
do_munmap(addr, length):
  1. 查找覆盖 [addr, addr+length) 的 VMA
  2. 对每个 VMA：
     - 调用 vmm_unmap_4k_page() 逐页释放
     - 若 VMA 被部分 unmap → 拆分 VMA
     - 若 VMA 完全在范围内 → 从链表移除 + kfree
     - 文件映射：vfs_node_put(vma->vm_file)
  3. tlb_shootdown()
  4. 返回 0
```

### 7. do_page_fault 增强 (`kernel/arch/x86_64/trap.c`)

当前用户态 PF 直接 `kill_current_user_task`。改为：

```
do_page_fault(regs, error_code) 用户态路径:
  // NOTE: do_page_fault 在 IST 栈上运行，不能使用 current。
  // 使用 task_from_tss() 获取被中断任务的 task_t。
  task_t *t = task_from_tss();
  cr2 = 故障地址（movq %%cr2, ...）

  // 无 mm（内核线程）→ SIGSEGV
  if (!t->mm): goto kill

  vma = vma_find(t->mm, cr2)
  if (!vma): goto kill

  // ── 权限检查 — error_code bit 含义 ──────────────────
  //   bit 0 (P): 0=页不存在, 1=保护违例
  //   bit 1 (W/R): 1=写操作
  //   bit 2 (U/S): 1=用户态
  //   bit 4 (I/D): 1=指令 fetch

  // PROT_NONE VMA → 任何访问都是 SIGSEGV
  if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))):
    goto kill

  // 写保护违例 (P=1, W=1) → 检查 VMA 写权限
  if ((error_code & 0x03) == 0x03 && !(vma->vm_flags & VM_WRITE)):
    goto kill

  // 指令 fetch (I=1) → 检查 VMA 执行权限
  if ((error_code & 0x10) && !(vma->vm_flags & VM_EXEC)):
    goto kill

  // ── 页不存在 (P=0) — 按需分配 ─────────────────────
  if (!(error_code & 0x01)):
    uint64_t phys;

    // 匿名映射首次访问
    if (vma->vm_flags & VM_ANON):
      phys = alloc_4k_page()
      if (!phys): 发送 SIGBUS → kill_current_user_task
      用 t->mm->pml4 获取 user_pml4 = Phy_To_Virt(t->mm->pml4)
      vmm_map_4k_page(user_pml4, phys,
                       PAGE_4K_ALIGN(cr2), vma->vm_page_prot)
      return

    // 文件映射首次访问
    // 局限性：vfs_read 在 AHCI/FAT 磁盘上会阻塞 I/O（wait_for_io_complete
    // → schedule），而 do_page_fault 在 IST 栈上 IF=0，不可调度。
    // 第一版仅支持内存型 VFS（如 initramfs ramdisk 上的 FAT）。
    // 面向 AHCI 的文件映射需后续 workqueue 支持。
    if (vma->vm_file):
      phys = alloc_4k_page()
      if (!phys): 发送 SIGBUS
      file_offset = (cr2 - vma->vm_start) + (vma->vm_pgoff << PAGE_4K_SHIFT)
      // vfs_read 第 3 个参数直接接受 offset（无需 vfs_lseek）
      int n = vfs_read(vma->vm_file, file_offset,
                        PAGE_4K_SIZE, (void *)Phy_To_Virt(phys))
      if (n < 0): free_4k_page(phys); 发送 SIGBUS
      vmm_map_4k_page(user_pml4, phys,
                       PAGE_4K_ALIGN(cr2), vma->vm_page_prot)
      return

  // ── COW fault ────────────────────────────────────
  // 本设计预留，后续 PR 实现。
  // if (P=1, W=1, page marked COW) → allocate + copy + remap r/w

kill:
  SIGSEGV → kill_current_user_task(regs)
```

**IST 栈上下文**：`do_page_fault` 使用 IST1 栈。`vma_find()`/`vmm_map_4k_page()` 等函数只操作数据结构，不依赖 `current` 宏。通过 `task_from_tss()` 获取 `task_t*`，从中提取 `mm`、`pml4` 等字段。`alloc_4k_page()` 内部使用 `spin_lock_irqsave`，IST 上下文 IF=0 下安全。

**VFS 可睡眠性约束**：文件映射的 `vfs_read` 在 do_page_fault 中调用。若文件系统后端需要磁盘 I/O（AHCI → `wait_for_io_complete` → `schedule()`），则 IST 上下文会崩溃（无有效 current、不可调度）。**第一版限制**：文件映射仅支持纯内存型 VFS（initramfs 上的 FAT/in-memory files）。实现时加运行时检查：若 `vma->vm_file->fs_type` 非内存型 → do_mmap 时直接返回 `-ENODEV`。后续 PR 可通过 workqueue 将磁盘文件页加载移至进程上下文。

### 8. 4KB 物理页分配 (`kernel/memory/pmm.c`)

**问题**：当前 `alloc_pages(zone, number, flags)` 的 `bits_map` 粒度为 2MB（`PAGE_2M_SHIFT`）。不支持 4KB 单页分配。

**方案**：新增 `alloc_4k_page()` / `free_4k_page()`，使用 2MB pool + 内部 bitmap 管理 512 个 4KB 子页。第一版限制：**仅支持分配，free_4k_page 为骨架**（munmap 暂不回收 4KB 子页到 pool——等 buddy allocator 后统一解决）。

```c
// kernel/memory/pmm.c
#define SUBPAGE_4K_COUNT (PAGE_2M_SIZE / PAGE_4K_SIZE)  // 512

// Pool list node — one 2MB page split into 512 4KB subpages
struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;         // 2MB page physical address
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    int         alloc_count;
};

static list_t subpage_pools = LIST_INIT(subpage_pools);
static spinlock_T subpage_lock = { .lock = 1L };

// Allocate a single 4KB physical page.  Returns physical address (NOT a
// struct Page*), or 0 on OOM.  Caller writes the address into a PTE immediately.
// SMP-safe: protected by subpage_lock (spin_lock_irqsave — safe from page fault
// context since PF runs with IF=0 and IRQs off means no other CPU locks).
//
// First fit across pools.  If no free slot, allocate a new 2MB pool.
uint64_t alloc_4k_page(void)
{
    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    // Search existing pools for a free slot
    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool = container_of(pos, struct subpage_pool, list);
        if (pool->alloc_count < SUBPAGE_4K_COUNT) {
            for (int i = 0; i < SUBPAGE_4K_COUNT / 64; i++) {
                if (pool->bitmap[i] == (uint64_t)-1) continue; // full word
                int bit = __builtin_ctzll(~pool->bitmap[i]);
                pool->bitmap[i] |= (1ULL << bit);
                pool->alloc_count++;
                uint64_t phys = pool->base_phys + (i * 64 + bit) * PAGE_4K_SIZE;
                spin_unlock_irqrestore(&subpage_lock, flags);
                return phys;
            }
        }
        pos = pos->next;
    }

    // No free slot — allocate a new 2MB pool
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, PG_PTable_Maped);
    if (!pg) {
        spin_unlock_irqrestore(&subpage_lock, flags);
        return 0;  // OOM
    }
    struct subpage_pool *pool = (struct subpage_pool *)
        Phy_To_Virt(pg->phy_address);  // use first bytes of the 2MB page
    list_init(&pool->list);
    pool->base_phys = pg->phy_address;
    memset(pool->bitmap, 0, sizeof(pool->bitmap));
    pool->alloc_count = 0;

    // Mark first slot as used (don't use subpage_pool struct itself)
    pool->bitmap[0] |= 1;
    // Mark the rest of the first page as used (potential subpage_pool growth)
    // Actually: subpage_pool is < 4KB, so just mark slot 0
    pool->alloc_count = 1;
    list_add(&pool->list, &subpage_pools);

    uint64_t phys = pool->base_phys + PAGE_4K_SIZE;  // slot 1 (skip pool struct)
    // bitmap slot 1
    pool->bitmap[0] |= (1ULL << 1);
    pool->alloc_count++;

    spin_unlock_irqrestore(&subpage_lock, flags);
    return phys;
}

// Free a 4KB page previously allocated by alloc_4k_page().
// First version: NO-OP skeleton.  Subpage slots are NOT returned to
// the pool.  munmap'd 4KB pages are leaked until the pool's 2MB page
// is fully freed (all 512 slots unused) — which requires a buddy
// allocator upgrade to track.
//
// TODO: implement slot reclaim when buddy allocator supports 4KB pages.
void free_4k_page(uint64_t phys)
{
    (void)phys;
    // Will implement: locate pool by base_phys range, clear bitmap slot,
    // if pool->alloc_count reaches 0 → list_del + free_pages 2MB.
}
```

**设计权衡**：
- 返回 `uint64_t` 而非 `struct Page*` — 调用方只需要物理地址写 PTE，无伪造 `Page` 结构 + 无竞态。
- `subpage_lock` 是 `spin_lock_irqsave` — `do_page_fault` 在 IST 栈上运行 IF=0，`spin_lock_irqsave` 不会开中断也不会死锁（只是保存/恢复 flags，锁本身是纯自旋）。
- `subpage_pool` 结构存放在 2MB 页的首个 4KB slot 中（`Phy_To_Virt(pg->phy_address)`），无需额外 `kmalloc`。
- **已知限制**：`free_4k_page` 第一版不回收 slot。munmap 一个 4KB 映射后物理页不返还 pool（相当于"分配不回收"语义）。这不影响正确性（页表项已清除，用户态无法再访问），只影响内存复用。待 buddy allocator 升级后统一解决。

### 9. VMA 生命周期

**fork**：`do_fork` → `fork_mm_copy` + `fork_vma_copy`。

`fork_vma_copy(parent_mm, child_mm)`：深拷贝 VMA 链表（每个 VMA `kmalloc` 副本）。文件映射需 `vfs_node_get(vm_file)` 增加引用计数，确保 child 的 VMA 持有独立引用，parent 的 munmap 不会过早释放 vfs_node。

**`fork_mm_copy` 对 4KB 映射的处理**（关键）：现有 `fork_mm_copy` 对 `PAGE_PS` 的 2MB 页做 eager copy，但对非 PS 条目直接 `child_pml2[l2] = pml2e; continue;`（共享 PML2 条目）。一旦 mmap 建立了 4KB PTE 表（PMD 条目无 PS，指向 PTE 表），fork 后父子共享同一张 PTE 表 → MAP_PRIVATE 页的写操作彼此可见，且任一方 munmap/exit 释放物理页会使另一方的页表项悬空。**本方案选择 eager copy 策略**：在 `fork_mm_copy` 中对非 PS 且 Present 的 PMD 条目，深拷贝其 PTE 表及所有已映射的 4KB 物理页。修改点如下：

```c
// kernel/sched/task.c — fork_mm_copy 中 PML2 循环内
if (!(pml2e & PAGE_PS)) {
    // 4KB PTE table: deep copy the PTE table and all mapped 4KB pages
    if (!(pml2e & PAGE_Present)) {
        child_pml2[l2] = 0;
        continue;
    }
    uint64_t *parent_pte = (uint64_t *)Phy_To_Virt(pml2e & PAGE_4K_MASK);
    uint64_t *child_pte  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
    if (!child_pte) { child_pml2[l2] = pml2e; continue; } // OOM: share
    child_pml2[l2] = Virt_To_Phy((uint64_t)child_pte) | (pml2e & 0xfff);
    for (int l1 = 0; l1 < 512; l1++) {
        uint64_t pte = parent_pte[l1];
        if (!(pte & PAGE_Present)) continue;
        uint64_t phys = pte & PAGE_4K_MASK;
        struct Page *s = alloc_pages(ZONE_NORMAL, 1, 0);
        if (s) {
            uint64_t dst = (uint64_t)Phy_To_Virt(s->phy_address);
            uint64_t src = (uint64_t)Phy_To_Virt(phys);
            __asm__ __volatile__("cld\n\trep movsb\n\t"
                : "+S"(src), "+D"(dst), "+c"(sz)
                : : "memory");
            child_pte[l1] = s->phy_address | (pte & ~PAGE_4K_MASK);
        } else {
            child_pte[l1] = pte; // OOM: share
        }
    }
    continue;
}
```

**exec**：`sys_exec` — 释放所有旧 VMA（`vma_free_all()`），但**不释放页表**（当前 sys_exec 只 kfree(current->mm) 不走 vmm_free_user_map，避免破坏父进程共享的页表）。mmap 引入后，exec 需要 `vma_release_all(mm)`：遍历所有 VMA → 释放 4KB 物理页 + PTE 表 + 2MB 页(仅匿名 VMA)，然后 `vma_free_all()` 释放 VMA 节点。文件映射的物理页需要特殊处理（只 unmap PTE，不 free 物理页——物理页已交还给 mm 引用计数。

**exit**：`do_exit` → `vma_release_all()` + `vma_free_all()` — 遍历链表释放所有 VMA、物理页、PTE 表。现有 `do_exit` 已跳过 `vmm_free_user_map`（`pml4` 可能共享），mmap 引入后改为只释放非共享资源：释放匿名 VMA 的物理页 + 所有 PTE 表，但 `pml4`/PDPT/PD 表本身和 `pml4` 分配页仍走现有"等 zombie 收割时再 free"的延迟路径。

### 10. mmap_base 初始化

`mm_t.mmap_base` 是 mmap 搜索空闲区的起始地址。`calloc` 默认 0 会导致首次 mmap 从 VA 0 开始搜索——VA 0 通常被保留为 NULL 区间。三个创建 `mm_t` 的位置需要显式初始化：

```c
// sys_exec: new_mm->mmap_base = 0x40000000  // 紧贴默认 ELF 加载地址之上
// spawn_user_task: mm->mmap_base = 0x40000000
// fork_mm_copy: memcpy 自动继承 parent 的 mmap_base
```

建议值 `0x40000000`（1GB 处）：高于 ELF 默认加载地址（`USER_CODE_ADDR = 0x400000`）和堆区，低于用户栈（`USER_STACK_BASE = 0x00007fffffffffff` 处的 2MB 页）。

### 11. 16KB 簇（可选内部策略）

作为 VMA 实现细节而非 ABI 约束：`do_mmap` 可将 `length` 向上对齐到 16KB 以增大 VMA 粒度、减少 VMA 节点数量，但 `vm_start`/`vm_end` 的 4KB 对齐保证不变。`do_page_fault` 仅分配触碰到的 4KB 页。第一版暂不实现 16KB 簇——保持 4KB 对齐，与 Linux ABI 一致。

## 新增 syscall 编号

| nr | 名称 | Linux 兼容 |
|----|------|-----------|
| 44 | `SYS_mmap` | Linux nr 9 → 映射到 OS01 nr 44 |
| 45 | `SYS_mprotect` | Linux nr 10 → 映射到 OS01 nr 45 |
| 46 | `SYS_munmap` | Linux nr 11 → 映射到 OS01 nr 46 |

Linux ABI 表更新：
```c
[9]  = 44,  // mmap
[10] = 45,  // mprotect
[11] = 46,  // munmap
```

## 不在范围内

- COW fault handler（`do_page_fault` 的写时拷贝路径——后续 PR）
- `MAP_SHARED` 文件映射的脏页回写（`msync` / `munmap` 写回）
- `MAP_GROWSDOWN` / `MAP_STACK` / `MAP_LOCKED`
- `mremap` / `madvise` / `mlock` / `msync`
- `/proc/<pid>/maps`（后续 PR）
- `PROT_EXEC` 细粒度控制（当前 ELF 加载已设 NX 位）

## VFS 文件映射前提

- `vfs_read(node, offset, size, buf)`（`kernel/fs/vfs.c`）第 3 个参数直接接受 offset，无需 `vfs_lseek`。`do_page_fault` 中直接 `vfs_read(vma->vm_file, file_offset, PAGE_4K_SIZE, Phy_To_Virt(phys))`。
- `do_mmap` 中需要 `vfs_node_get(vm_file)` 增加引用计数；`do_munmap` 中 `vfs_node_put(vm_file)`；`fork_vma_copy` 对文件型 VMA `vfs_node_get`。
- **VFS 可睡眠性**（见 §7）：文件映射第一版仅支持内存型 VFS。实现时在 do_mmap 中检查 `vm_file` 所在 fs 是否 AHCI-backed，若是则返回 `-ENODEV`。后续 workqueue PR 解除此限制。

## 文件变更清单

| 文件 | 操作 | 内容 |
|------|------|------|
| `kernel/include/kernel/vma.h` | 新建 | `vma_t` 结构、`VM_*` 常量、`vma_find`/`vma_insert`/`vma_remove`/`vma_free_all` 声明 |
| `kernel/memory/vma.c` | 新建 | VMA 链表操作、`do_mmap`、`do_mprotect`、`do_munmap` |
| `kernel/memory/vmm.c` | 修改 | 新增 `vmm_map_4k_page`、`vmm_unmap_4k_page`、`vmm_pt_walk`、PAGE_USER_4K / PAGE_USER_4K_RO 宏 |
| `kernel/include/kernel/vmm.h` | 修改 | 新增 4KB 函数声明和 PTE 级标志宏 |
| `kernel/include/kernel/task.h` | 修改 | `mm_t` 新增 `vma_list`、`mmap_base` |
| `kernel/arch/x86_64/trap.c` | 修改 | `do_page_fault` 用户态路径重构（VMA 查找+按需分配+按需读文件）；`do_system_call` 新增 3 个 case（读取 regs->r8/r9/r10）；Linux ABI 表 nr 9/10/11 启用 |
| `kernel/sched/task.c` | 修改 | `fork_mm_copy` 新增 PTE 表深拷贝路径（非 PS 条目的 4KB 页 eager copy）；`do_exit` 调用 `vma_release_all`；`sys_exec` 调用 `vma_release_all` + 初始化 `mmap_base` |
| `kernel/memory/pmm.c` | 修改 | 新增 `alloc_4k_page()`（返回 `uint64_t` 物理地址，SMP-safe spinlock + pool list）、`free_4k_page()`（骨架 no-op）；pool 结构存于 2MB 页首部 |
| `kernel/include/kernel/pmm.h` | 修改 | 新增 `alloc_4k_page`/`free_4k_page` 声明（`uint64_t alloc_4k_page(void)`） |
| `libc/include/sys/syscall.h` | 修改 | 新增 `SYS_mmap`/`SYS_mprotect`/`SYS_munmap` nr + `syscall6()` 宏 |
| `libc/include/sys/mman.h` | 新建 | `PROT_*`、`MAP_*` 常量、`mmap`/`munmap`/`mprotect` 声明 |
| `libc/unistd/mmap.c` | 新建 | `mmap`/`munmap` 实现（`mprotect` 如需要也可单独文件） |
| `libc/unistd/busybox_stubs.c` | 修改 | 移除 mmap/munmap stub，改为实际实现 |
| `kernel/Makefile` | 修改 | 添加 `memory/vma.c` |
| `libc/Makefile` | 修改 | 添加 `unistd/mmap.c` |

## 验证

1. **systest 保持 70/70** — 现有系统测试全部通过
2. **匿名映射测试** — 新 systest：`mmap(ANON)` → 写入 → 读取 → `munmap` → 验证数据
3. **mprotect 测试** — `mmap` → `mprotect(PROT_NONE)` → 访问触发 SIGSEGV
4. **busybox ash 保持正常** — 启动 shell 交互不受影响
5. **busybox grep** — `echo hello | grep h` 正常工作（文件映射 + 匿名映射）
6. **16KB 对齐** — 验证 12KB 请求实际分配 16KB，但仅触碰页分配物理内存
