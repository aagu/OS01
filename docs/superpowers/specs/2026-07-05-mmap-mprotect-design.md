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
    uint64_t vm_start;    // 起始 VA（4KB 对齐）
    uint64_t vm_end;      // 结束 VA（4KB 对齐）
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
#define PAGE_USER_4K      (PAGE_U_S | PAGE_R_W | PAGE_Present)   // 用户 R/W 4KB
#define PAGE_USER_4K_RO   (PAGE_U_S | PAGE_Present)              // 用户只读 4KB
#define PAGE_KERNEL_4K    (PAGE_R_W | PAGE_Present)              // 内核 4KB
#define PAGE_PROTNONE     (1UL << 9)   // 软件位：PROT_NONE 暂存标记
// bit 9 是 x86_64 PTE ignored 位。mprotect(PROT_NONE) 在 PTE 上设此位，清 Present
// 但保留 phys。mprotect(PROT_READ) 遍历 PTE 恢复 Present 并清除此位。do_munmap/
// vma_free_all 检测此位知道 phys 有效需 free_4k_page。do_page_fault 不做此位处理
// ——PROT_NONE 期间 VMA 的 vm_flags 已清零，在权限检查阶段就 SIGSEGV 了。
```

新增函数（`vmm.c` / `vmm.h`）：

- **`int vmm_map_4k_page(pagemap, phys, virt, flags)`** — 调用 `vmm_pt_walk(pagemap, virt, flags, true)` 获取 PTE 条目指针。若返回 NULL → 返回 `-ENOMEM`（PTE 表分配失败）。成功则在 PTE[level1] 写入 `phys | flags`（不含 `PAGE_PS`），返回 0。调用方（`do_page_fault`）必须检查返回值：失败则 `free_4k_page(phys)` + SIGBUS。
- **`void vmm_unmap_4k_page(pagemap, virt)`** — 调用 `vmm_pt_walk(pagemap, virt, 0, false)`。判定：若返回 NULL → 直接 return（PTE 表不存在）。若 `*pte` 既无 `PAGE_Present` 也无 `PAGE_PROTNONE` → return（真正未映射的页，demand-paging 正常路径）。否则 `phys = *pte & PAGE_4K_MASK`，清 PTE，`free_4k_page(phys)`。**关键**：PROT_NONE 页是 Present=0 + PROTNONE=1 + phys 保留，不加 PROTNONE 检查会当成未映射跳过，phys 泄漏。
- **`uint64_t *vmm_pt_walk(pagemap, virt, flags, allocate)`** — 遍历 PML4→PML3→PML2→PTE。`flags` 含 `PAGE_U_S` 时中间层用 `PAGE_USER_GDT/Dir`，否则 `PAGE_KERNEL_GDT/Dir`。`allocate=true` 且某层不存在时 `calloc` 分配；若 `calloc` 失败 → 返回 `NULL`（OOM）。返回指向 PTE[level1] 条目的指针，供读写。

### 3. syscall6() 宏 + 参数映射 (`libc/include/sys/syscall.h`)

mmap 需要 6 个参数，现有 `syscall()` 只传 3 个（`%rdi, %rsi, %rdx`）。新增 `syscall6()`：

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

**mmap 参数映射表**（libc → int $0x80 → 内核 pt_regs）：

| libc syscall6 形参 | 寄存器 | 内核 regs 字段 | mmap 语义 |
|---------------------|--------|---------------|-----------|
| `arg1` | `%rdi` | `regs->rdi` | `addr` |
| `arg2` | `%rsi` | `regs->rsi` | `length` |
| `arg3` | `%rdx` | `regs->rdx` | `prot` |
| `arg4` | `%r10` | `regs->r10` | `flags` |
| `arg5` | `%r8`  | `regs->r8`  | `fd` |
| `arg6` | `%r9`  | `regs->r9`  | `offset` |

**libc mmap 包装** (`libc/unistd/mmap.c`)：

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    int64_t ret = syscall6(SYS_mmap,
                           (uint64_t)addr, (uint64_t)length,
                           (uint64_t)prot, (uint64_t)flags,
                           (uint64_t)fd, (uint64_t)offset);
    if (ret < 0 && ret > -4096) {  // Linux 风格：负数且 > -4096 是 errno
        errno = (int)(-ret);
        return MAP_FAILED;
    }
    return (void *)ret;
}
```

**`libc/include/sys/mman.h` 常量清单**：

```c
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_FAILED  ((void *)-1)
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);
```

**Linux ABI 兼容性**：busybox 走 OS01 libc 的 `mmap()` → `syscall6(SYS_mmap, ...)`。`do_system_call` 中的 `linux_to_os01` 表启用 `[9]=44`、`[10]=45`、`[11]=46` 主要是完整性保障——如果 busybox 未来绕过 libc 直接发 Linux nr 也能正确映射。

### 4. do_mmap (`kernel/memory/vma.c`)

```
do_mmap(addr, length, prot, flags, fd, offset):
  1. 参数校验
     - length == 0 → -EINVAL
     - 未知 prot 位 → -EINVAL
     - PROT_WRITE 无 PROT_READ → -EINVAL（x86 不支持只写）
     - PROT_EXEC 无 PROT_READ → 隐式加 PROT_READ（x86 不可执行不可读页）
     - offset 非 PAGE_4K_SIZE 对齐 → -EINVAL
     - end = PAGE_4K_ALIGN(addr + length)，若 end 溢出回绕 → -EINVAL
     - MAP_ANONYMOUS 必须有 fd == -1 → -EINVAL
     - 非 MAP_ANONYMOUS 必须 fd 有效且 fd 对应 vfs_node 非 NULL → -EBADF
     - MAP_FIXED 且 addr 非 4KB 对齐 → -EINVAL
     - MAP_FIXED 且 addr >= current->addr_limit → -ENOMEM
  
  2. 地址计算
     - 若 MAP_FIXED：使用 addr
       重叠 VMA 先 munmap（调用 do_munmap 清理 [addr, addr+length) 内的现存 VMA）
     - 否则：从 mm->mmap_base 开始向上搜索空闲区间
     - 长度对齐到 4KB 上界

  3. flags 翻译
     prot → vm_page_prot:
       PROT_READ        → PAGE_USER_4K_RO
       PROT_READ|WRITE  → PAGE_USER_4K
       PROT_EXEC        → PAGE_USER_4K（暂不设 NX，与现有 ELF PAGE_USER_Page 一致）
       PROT_NONE        → PAGE_U_S（无 Present + 无 R/W，VMA 的 vm_flags 全 0）
     vm_flags: MAP_SHARED → VM_SHARED, MAP_PRIVATE → 0
                MAP_ANONYMOUS → VM_ANON
  
  4. 文件映射：vfs_node_get(vm_file) 增加引用计数
     （V1 无 AHCI 限制——AHCI 是 busy-poll 无 schedule()，IST 上下文安全。见 §7）
  
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
     范围内有 VMA 之间的空洞 → -ENOMEM（Linux 语义：整个区间都必须已映射）
  3. 对每个 VMA：
     - 更新 vm_page_prot 和 vm_flags（同步 VM_READ/VM_WRITE/VM_EXEC）
       PROT_NONE → vm_flags 清除 VM_READ|VM_WRITE|VM_EXEC
       PROT_READ  → vm_flags |= VM_READ，vm_page_prot = PAGE_USER_4K_RO
       以此类推
     - 对已有物理页：遍历 PTE 条目，更新权限位
       PROT_NONE → 清 PAGE_Present + PAGE_R/W，保留 phys，**设 PAGE_PROTNONE**（bit 9）
       PROT_READ → 从 PROT_NONE 恢复时，清 PAGE_PROTNONE，置 PAGE_Present + PAGE_USER_4K_RO
     - 用户页表修改：本地 flush_tlb() 即可
  4. 返回 0
```

### 6. do_munmap

```
do_munmap(addr, length):
  1. 查找覆盖 [addr, addr+length) 的 VMA
  2. 对每个 VMA：
     - 调用 vmm_unmap_4k_page() 逐页释放
     - 若 VMA 被部分 unmap → 拆分 VMA
       拆分后新建 VMA 节点继承 vm_file：需要 vfs_node_get(vm_file)
       （否则原 VMA 被 vfs_node_put 后新节点的 vm_file 悬空）
     - 若 VMA 完全在范围内 → 从链表移除 + kfree
     - 文件映射：vfs_node_put(vma->vm_file)
  3. flush_tlb()
  4. 返回 0
```

### 7. do_page_fault 增强 (`kernel/arch/x86_64/trap.c`)

当前用户态 PF 直接 `kill_current_user_task`，内核态 PF halt。重构后保留两路分流：

```
do_page_fault(regs, error_code):
  // ── 内核态 PF: 保持原 halt 路径 ─────────────────
  // 不能进 vma_find→kill_current_user_task（会把内核 bug 误当用户 SIGSEGV）
  if (!(regs->cs & 3)):
    原内核 PF 逻辑（dump CR2/error/regs → backtrace → halt）
    return

  // ── 用户态 PF ─────────────────────────────────
  // NOTE: do_page_fault 在 IST1 栈上，不能使用 current。
  task_t *t = task_from_tss();     // 可返回 NULL（cpu/tss/rsp0 无效时）
  if (!t || !t->mm): goto kill     // NULL guard + 内核线程无用户空间
  cr2 = 故障地址（movq %%cr2, ...）

  vma = vma_find(t->mm, cr2)
  if (!vma): goto kill

  // ── 权限检查 ──────────────────────────────────
  // error_code: bit 0=P, bit 1=W/R, bit 2=U/S, bit 4=I/D

  // PROT_NONE VMA → 任何访问都是 SIGSEGV
  if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))):
    goto kill

  // 写保护违例 (P=1, W=1) → 检查 VMA 写权限
  if ((error_code & 0x03) == 0x03 && !(vma->vm_flags & VM_WRITE)):
    goto kill

  // 指令 fetch (I=1) → 检查 VMA 执行权限
  if ((error_code & 0x10) && !(vma->vm_flags & VM_EXEC)):
    goto kill

  // ── 页不存在 (P=0) — 按需分配 ─────────────────
  if (!(error_code & 0x01)):
    user_pml4 = (uint64_t *)Phy_To_Virt(t->mm->pml4)
    uint64_t phys;

    // 匿名映射首次访问
    if (vma->vm_flags & VM_ANON):
      phys = alloc_4k_page()
      if (!phys): 发送 SIGBUS → goto kill
      int rc = vmm_map_4k_page(user_pml4, phys, PAGE_4K_ALIGN(cr2),
                                vma->vm_page_prot);
      if (rc != 0): free_4k_page(phys); 发送 SIGBUS → goto kill
      return

    // 文件映射首次访问
    // AHCI 是 busy-poll（ahci_read_sectors 轮询 port->ci + jiffies 超时），
    // 整条路径无 schedule()。IST 栈上调 vfs_read 安全。
    // IF 保持用户态值（通常 1），busy-wait 期间 IRQ 仍可触发。
    if (vma->vm_file):
      phys = alloc_4k_page()
      if (!phys): 发送 SIGBUS
      file_offset = (cr2 - vma->vm_start)
                  + (vma->vm_pgoff << PAGE_4K_SHIFT)
      int n = vfs_read(vma->vm_file, file_offset,
                        PAGE_4K_SIZE, (void *)Phy_To_Virt(phys))
      if (n < 0): free_4k_page(phys); 发送 SIGBUS
      rc = vmm_map_4k_page(user_pml4, phys, PAGE_4K_ALIGN(cr2),
                            vma->vm_page_prot);
      if (rc != 0): free_4k_page(phys); 发送 SIGBUS
      return

  // ── COW fault（后续 PR） ─────────────────────
  // if (P=1, W=1, page marked COW) → allocate + copy + remap r/w

kill:
  SIGSEGV → kill_current_user_task(regs)
```

**IST 栈上下文**：`do_page_fault` 使用 IST1 栈。`vma_find()`/`vmm_map_4k_page()` 等函数只操作数据结构，不依赖 `current` 宏。`entry.S` 的 `page_fault` 入口不修改 IF（用户态进来通常 IF=1）；`alloc_4k_page()` 内部用 `spin_lock_irqsave` 自行管理中断屏蔽，安全。

**IST1 栈深度警告**：重构后 `do_page_fault` 新增了 `vmm_pt_walk`（→ `calloc` → slab 多级调用）、`alloc_4k_page`（→ `spin_lock`）、文件路径的 `vfs_read`（→ `fat_read` → `ahci_read_sectors` busy-poll）。原 PF handler 仅 `printk` + `backtrace`。实现后需验证 IST1 栈有余量（`head.S` 中 `IST1_SIZE` = 7 页 = 28KB），建议在 do_page_fault 入口读 RSP 与 IST1 栈底比较，差值 < 4KB 时 `serial_printk` 告警（不给优化）。后续可将文件读迁移到 workqueue 上下文以减小 IST 压力。

**AHCI 文件映射可行性**：`ahci_read_sectors`（`kernel/driver/ahci.c:487`）是 busy-poll（轮询 `port->ci` 位 + jiffies 超时），整条 I/O 路径无 `schedule()`。`vfs_read` 在 IST 上下文是安全的——不会因调度崩溃。V1 直接支持 AHCI 文件映射，不设 `-ENODEV` 限制。

### 8. 4KB 物理页分配 (`kernel/memory/pmm.c`)

**问题**：当前 `alloc_pages(zone, number, flags)` 的 `bits_map` 粒度为 2MB（`PAGE_2M_SHIFT`）。不支持 4KB 单页分配。

**方案**：新增 `alloc_4k_page()` / `free_4k_page()`，使用 2MB pool + 内部 bitmap 管理 512 个 4KB 子页。支持分配和回收，同时满足正确性和 busybox 高频 mmap/munmap 的长跑需求。

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

static list_t subpage_pools;             // initialized in pmm_init()
static spinlock_T subpage_lock = { .lock = 1L };

// In pmm_init() or a subpage_pool_init() called after slab_init():
//     list_init(&subpage_pools);

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
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);  // 0=no PG_PTable_Maped
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
    list_add_to_behind(&subpage_pools, &pool->list);

    uint64_t phys = pool->base_phys + PAGE_4K_SIZE;  // slot 1 (skip pool struct)
    // bitmap slot 1
    pool->bitmap[0] |= (1ULL << 1);
    pool->alloc_count++;

    spin_unlock_irqrestore(&subpage_lock, flags);
    return phys;
}

// Free a 4KB page previously allocated by alloc_4k_page().
// Locates the owning pool by phys range, clears the bitmap slot,
// and reclaims the slot for future alloc_4k_page() calls.
// SMP-safe: subpage_lock is held.
void free_4k_page(uint64_t phys)
{
    if (!phys) return;

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool = container_of(pos, struct subpage_pool, list);

        // Does phys fall within this pool?
        if (phys >= pool->base_phys &&
            phys < pool->base_phys + PAGE_2M_SIZE) {

            uint64_t offset = phys - pool->base_phys;
            int slot = (int)(offset / PAGE_4K_SIZE);
            if (slot < 0 || slot >= SUBPAGE_4K_COUNT) break; // shouldn't happen
            int word = slot / 64;
            int bit  = slot % 64;

            if (pool->bitmap[word] & (1ULL << bit)) {
                pool->bitmap[word] &= ~(1ULL << bit);
                pool->alloc_count--;
            }
            // NOTE: pool's 2MB page is NOT freed when alloc_count
            // reaches 0.  It stays in the list for future use.
            // Freeing the 2MB back to PMM requires tracking whether
            // the 2MB page is still mapped in any page table — a
            // refcount that depends on the buddy allocator upgrade.
            // This is fine: the pool page acts as a cache, not a
            // leak, since slots are now recycled.

            spin_unlock_irqrestore(&subpage_lock, flags);
            return;
        }
        pos = pos->next;
    }

    // phys not found in any pool — shouldn't happen unless caller
    // passes a wrong address.
    spin_unlock_irqrestore(&subpage_lock, flags);
}
```

**设计权衡**：
- 返回 `uint64_t` 而非 `struct Page*` — 调用方只需要物理地址写 PTE，无伪造 `Page` 结构 + 无竞态。
- `subpage_lock` 是 `spin_lock_irqsave` — `spin_lock_irqsave` 自行保存并清 IF，与调用上下文（`#PF` 入口不修改 IF，用户态进来通常 IF=1）无关。锁操作安全。
- `subpage_pool` 结构存放在 2MB 页的首个 4KB slot 中（`Phy_To_Virt(pg->phy_address)`），无需额外 `kmalloc`。
- `subpage_pools` 是全局链表，需在 `pmm_init()` → `slab_init()` 之后调用 `list_init(&subpage_pools)` 初始化。
- **已知限制**：pool 的 2MB 页在 `alloc_count` 归零后不会归还 PMM（无法判断该 2MB 页是否仍被某个页表引用，需 refcount 追踪）。这不影响 busybox 长跑——4KB slot 已回收复用。

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
        // Copy both Present and PROTNONE pages.  PROTNONE pages have
        // Present=0 but a valid phys address — skip them here and the
        // child loses the data after parent's mprotect(PROT_READ).
        if (!(pte & (PAGE_Present | PAGE_PROTNONE))) continue;

        uint64_t parent_phys = pte & PAGE_4K_MASK;
        uint64_t child_phys = alloc_4k_page();
        if (child_phys) {
            memcpy((void *)Phy_To_Virt(child_phys),
                   (void *)Phy_To_Virt(parent_phys), PAGE_4K_SIZE);
            // Preserve PROTNONE bit: child gets same Present/PROTNONE state
            child_pte[l1] = child_phys | (pte & ~PAGE_4K_MASK);
        } else {
            child_pte[l1] = pte; // OOM: share
        }
    }
    continue;
}
```

**exec**：`sys_exec` — 调用 `vma_free_all(mm)`（一次遍历完成两步）：释放 VMA 覆盖的 4KB 物理页（匿名）+ PTE 表，然后释放 VMA 节点 + 文件映射的 `vfs_node_put`。与现有 `kfree(current->mm)` 的语义一致（只释放 mm 元数据，不递归释放共享的 pml4/PDPT/PD 表）。

**exit**：`do_exit` → `vma_free_all(mm)` — 同上。现有 `do_exit` 已跳过 `vmm_free_user_map`（`pml4` 可能共享），mmap 引入后保持该模式：释放 VMA 管理的用户页 + PTE 表，pml4/PDPT/PD 表本身走 zombie 收割的延迟 free 路径。

**ELF 2MB 页不在 VMA 中**：`sys_exec` 加载的 ELF code/data/stack 使用 `PAGE_PS` 2MB 大页，直接写在 pml4→pml3→pml2 中，不受 `vma_list` 跟踪。`vma_free_all` 只释放 VMA 管理的 4KB 页 + PTE 表，不触碰 2MB ELF 段页。2MB 页的释放继续沿用现有路径（`do_exit` 跳过 `vmm_free_user_map` → zombie 收割时处理）。后续 COW/fork 实现时，若 ELF 段也纳入 VMA 跟踪，这一分离需要重新评估。

### 10. mm_t 新字段初始化

`vma_list` 和 `mmap_base` 是 mm_t 的两个新字段。三个创建 `mm_t` 的位置需要初始化：

```c
// sys_exec (task.c ~893):      new_mm = calloc → list_init(&new_mm->vma_list);
//                               new_mm->mmap_base = 0x40000000;
// spawn_user_task (task.c ~689): mm = calloc → list_init(&mm->vma_list);
//                                 mm->mmap_base = 0x40000000;
// fork_mm_copy: memcpy(child_mm, parent_mm, sizeof(mm_t)) 会复制 parent
//               的 vma_list 指针 → 必须覆盖为 list_init(&child_mm->vma_list)
//               （vma_list 不能共享，fork_vma_copy 会填充 child 自己的 VMA 节点）
```

**重要**：`calloc` 出的 `vma_list` 是 NULL/NULL，`list_init` 必须显式调用。`fork_mm_copy` 的 `memcpy` 会把 parent 的 `vma_list` 指针（指向 parent 的 VMA 节点）拷到 child，必须在 `memcpy` 后重新 `list_init` + 再 `fork_vma_copy` 填充 child 版。

`mmap_base` 取值 `0x40000000`（1GB 处）：高于 ELF 加载地址（`USER_CODE_ADDR = 0x400000`）、堆区（brk 上限 ~0x600000）、用户栈（`USER_STACK_BASE = 0x800000` / `USER_STACK_TOP = 0x9FFFF0`），远低于 `addr_limit = 0x00007FFFFFFFFFFF`。该区域足够大（~1.75GB 到 addr_limit），适宜作为 mmap 默认搜索起点。

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

- `vfs_read(node, offset, size, buf)`（`kernel/fs/vfs.c`）第 3 个参数直接接受 offset。`do_page_fault` 中直接调用。
- `do_mmap` 中 `vfs_node_get(vm_file)` 增加引用计数；`do_munmap` 中 `vfs_node_put(vm_file)`；`fork_vma_copy` 对文件型 VMA `vfs_node_get`。
- **AHCI 兼容**：`ahci_read_sectors` 是 busy-poll（无 `schedule()`），IST 栈上 `vfs_read` 安全。V1 直接支持，不设文件系统后端限制。

## 文件变更清单

| 文件 | 操作 | 内容 |
|------|------|------|
| `kernel/include/kernel/vma.h` | 新建 | `vma_t` 结构、`VM_*` 常量、`vma_find`/`vma_insert`/`vma_remove`/`vma_free_all` 声明 |
| `kernel/memory/vma.c` | 新建 | VMA 链表操作、`do_mmap`、`do_mprotect`、`do_munmap`、`vma_free_all` |
| `kernel/memory/vmm.c` | 修改 | 新增 `vmm_map_4k_page`、`vmm_unmap_4k_page`、`vmm_pt_walk`、PAGE_USER_4K / PAGE_USER_4K_RO / PAGE_PROTNONE 宏 |
| `kernel/include/kernel/vmm.h` | 修改 | 新增 4KB 函数声明、PTE 级标志宏、PAGE_PROTNONE |
| `kernel/include/kernel/task.h` | 修改 | `mm_t` 新增 `vma_list`、`mmap_base` |
| `kernel/arch/x86_64/trap.c` | 修改 | `do_page_fault`: cs&3 双路分流（内核 halt，用户 VMA+按需分配）；`do_system_call` 新增 3 case（从 regs->r8/r9/r10 取 mmap 后三参）；Linux ABI [9/10/11] |
| `kernel/sched/task.c` | 修改 | `fork_mm_copy`: 非 PS 条目 PTE 表 deep copy（4KB eager）；`fork_vma_copy`；`do_exit`→`vma_free_all`；`sys_exec`→`vma_free_all` + `mmap_base=0x40000000`；`spawn_user_task`→`mmap_base` init |
| `kernel/memory/pmm.c` | 修改 | 新增 `alloc_4k_page()`（返回 `uint64_t`，SMP-safe spinlock + pool list）、`free_4k_page()`（按 phys 反查 pool → bitmap 清位 + slot 回收）；pool 结构存于 2MB 页首部；`pmm_init` 末尾 `list_init(&subpage_pools)` |
| `kernel/include/kernel/pmm.h` | 修改 | 新增 `uint64_t alloc_4k_page(void)` / `void free_4k_page(uint64_t phys)` |
| `libc/include/sys/syscall.h` | 修改 | 新增 `SYS_mmap=44`/`SYS_mprotect=45`/`SYS_munmap=46` + `syscall6()` 宏 + mmap 参数映射表 |
| `libc/include/sys/mman.h` | 新建 | `PROT_NONE/READ/WRITE/EXEC`、`MAP_FAILED/SHARED/PRIVATE/FIXED/ANONYMOUS`、函数声明 |
| `libc/unistd/mmap.c` | 新建 | `mmap`/`munmap`（`mmap` 用 `syscall6`，负返回值→`errno` + `MAP_FAILED`） |
| `libc/unistd/busybox_stubs.c` | 修改 | 移除 mmap/munmap stub |
| `kernel/Makefile` | 修改 | 添加 `memory/vma.c` |
| `libc/Makefile` | 修改 | 添加 `unistd/mmap.c` |

## 验证

1. **systest 保持 70/70** — 现有系统测试全部通过
2. **匿名映射测试** — 新 systest：`mmap(ANON)` → 写入 → 读取 → `munmap` → 验证数据
3. **mprotect 测试** — `mmap` → 写入数据 → `mprotect(PROT_NONE)` → 访问触发 SIGSEGV → `mprotect(PROT_READ)`（mprotect 遍历 PTE 同步恢复 Present）→ 读取验证原数据未丢失
4. **busybox ash 保持正常** — 启动 shell 交互不受影响
5. **busybox grep** — `echo hello | grep h` 正常工作（文件映射 + 匿名映射）
6. **fork + mmap** — `fork()` 后在父进程中 mmap 匿名页，父子各自写入不同值，读取验证隔离
