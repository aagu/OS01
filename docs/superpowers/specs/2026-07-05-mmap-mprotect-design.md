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
| 2 | 逻辑分配粒度 | 16KB 簇（4 个 4KB 页） | 减少页表管理开销，比 2MB 更节省内存 |
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
        : "r10", "r8", "r9", "memory");
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
     - MAP_ANONYMOUS 必须有 fd == -1 → -EINVAL
     - 非 MAP_ANONYMOUS 必须 fd 有效 → -EBADF
  
  2. 地址计算
     - 若 MAP_FIXED：使用 addr（页面对齐）
     - 否则：从 mm->mmap_base 开始搜索空闲区间
     - 长度对齐到 16KB 上界
  
  3. flags 翻译
     prot → vm_page_prot:
       PROT_READ  → PAGE_USER_4K_RO   // R/O: U/S + Present
       PROT_READ|PROT_WRITE → PAGE_USER_4K  // R/W: U/S + R/W + Present
       PROT_WRITE（无 READ）→ -EINVAL       // PROT_WRITE 无 PROT_READ 非法
       PROT_EXEC  → PAGE_USER_4K（暂不设 NX）
       PROT_NONE  → PAGE_U_S（无 Present，首次访问 #PF）
     vm_flags: MAP_SHARED → VM_SHARED, MAP_PRIVATE → 0
                MAP_ANONYMOUS → VM_ANON
                MAP_FIXED → 重叠 VMA 先 munmap
  
  4. 分配 vma_t（kmalloc）
     填充字段，插入 mm->vma_list（按 vm_start 排序，合并相邻兼容 VMA）
  
  5. 返回 vm_start
```

### 5. do_mprotect

```
do_mprotect(addr, length, prot):
  1. addr 4KB 对齐，length 上界对齐
  2. 遍历 addr..addr+length 覆盖的 VMA
  3. 对每个 VMA：
     - 更新 vm_page_prot
     - 对已有物理页：逐个 PTE 更新权限位
     - prot == PROT_NONE → 清 PAGE_Present（保留物理页，再次访问恢复权限）
  4. 若有修改 PTE → tlb_shootdown()
  5. 返回 0
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
  vma = vma_find(t->mm, cr2)        // 无 mm（内核线程）→ SIGSEGV
  
  if (!vma):
    SIGSEGV → kill_current_user_task(regs)
    return
  
  // 权限检查 — error_code bit 含义：
  //   bit 0 (P): 0=页不存在, 1=保护违例
  //   bit 1 (W/R): 1=写操作
  //   bit 2 (U/S): 1=用户态
  //   bit 4 (I/D): 1=指令 fetch
  
  // 写保护违例 (P=1, W=1) or 写入只读 VMA
  if ((error_code & 0x02) && !(vma->vm_flags & VM_WRITE)):
    SIGSEGV → kill_current_user_task(regs)
    return
  
  // 页不存在 (P=0) — 按需分配
  if (!(error_code & 0x01)):
    // 匿名映射首次访问
    if (vma->vm_flags & VM_ANON):
      page = alloc_4k_page()        // 见 §8
      if (!page): 发送 SIGBUS
      用 t->mm->pml4 获取 user_pml4 = Phy_To_Virt(t->mm->pml4)
      vmm_map_4k_page(user_pml4, page->phy_address,
                       PAGE_4K_ALIGN(cr2), vma->vm_page_prot)
      return
    
    // 文件映射首次访问
    if (vma->vm_file):
      page = alloc_4k_page()
      if (!page): 发送 SIGBUS
      file_offset = (cr2 - vma->vm_start) + (vma->vm_pgoff << PAGE_4K_SHIFT)
      vfs_lseek(vma->vm_file, file_offset, SEEK_SET)
      vfs_read(vma->vm_file, Phy_To_Virt(page->phy_address), PAGE_4K_SIZE)
      vmm_map_4k_page(user_pml4, page->phy_address,
                       PAGE_4K_ALIGN(cr2), vma->vm_page_prot)
      return
  
  // COW fault（本设计预留，后续 PR 实现）
  // if (P=1, W=1, page marked COW) → allocate + copy + remap r/w
  
  // 其他情况
  SIGSEGV → kill_current_user_task(regs)
```

**IST 栈上下文**：`do_page_fault` 使用 IST1 栈。`vma_find()`/`vmm_map_4k_page()` 等函数只操作数据结构，不依赖 `current` 宏。通过 `task_from_tss()` 获取 `task_t*`，从中提取 `mm`、`pml4` 等字段。

### 8. 4KB 物理页分配 (`kernel/memory/pmm.c`)

**问题**：当前 `alloc_pages(zone, number, flags)` 的 `bits_map` 粒度为 2MB（`PAGE_2M_SHIFT`），`number` 参数必须 > 0（`free_pages` 校验）。不支持 4KB 单页分配。

**方案**：新增 `alloc_4k_page()` 包装器，第一版使用预分配 2MB 页 + 内部 bitmap 管理 4KB 子页：

```c
// kernel/memory/pmm.c
#define SUBPAGE_4K_COUNT (PAGE_2M_SIZE / PAGE_4K_SIZE)  // 512

static struct Page *subpage_pool_page = NULL;  // 当前从属的 2MB 页
static uint64_t     subpage_bitmap[SUBPAGE_4K_COUNT / 64] = {0}; // 已用位
static int          subpage_alloc_count = 0;

struct Page *alloc_4k_page(void)
{
    // 若 pool 不存在或满了，申请新 2MB 页
    if (!subpage_pool_page || subpage_alloc_count >= SUBPAGE_4K_COUNT) {
        subpage_pool_page = alloc_pages(ZONE_NORMAL, 1, PG_PTable_Maped);
        if (!subpage_pool_page) return NULL;
        memset(subpage_bitmap, 0, sizeof(subpage_bitmap));
        subpage_alloc_count = 0;
    }
    // 在 bitmap 中查找空闲 4KB slot
    int slot = find_first_zero_bit(subpage_bitmap, SUBPAGE_4K_COUNT);
    bitmap_set(subpage_bitmap, slot);
    subpage_alloc_count++;
    // 返回伪造的 Page 结构（仅 phy_address 字段有效）
    static struct Page fake_page;
    fake_page.phy_address = subpage_pool_page->phy_address + slot * PAGE_4K_SIZE;
    return &fake_page;  // 调用者立即使用，下次调用覆盖
}

void free_4k_page(struct Page *page)
{
    // 计算 slot → bitmap_clear
    // subpage_alloc_count--
    // 若归零 → free_pages(subpage_pool_page, 1)
}
```

**限制**：`alloc_4k_page()` 返回的 `fake_page` 仅供立即使用（调用者在 `do_page_fault` 中立即读取 `phy_address` 然后写入 PTE），不支持跨调用持有。后续 COW fork 完成后可以升级为真正的 buddy 4KB 分配器。

**替代方案**（远期）：重构 PMM 的 `bits_map` 到 4KB 粒度，支持 `alloc_pages(ZONE_NORMAL, 1, flags)` 分配 1 个 4KB 页。这需要改动 `pmm_init` 中 `bits_size` 和 `bits_length` 的计算方式（8 倍位图增长）。当前影响范围太大，第一版先用 2MB pool 方案。

### 9. VMA 生命周期

- **fork**：`do_fork` → `fork_vma_copy(parent_mm, child_mm)` — 深拷贝 VMA 链表（每个 VMA `kmalloc` 副本），文件映射 `vfs_node_put` 替换为引用计数增加（若 VFS 支持）。
- **exec**：`sys_exec` — 释放所有旧 VMA（`vma_free_all()` ），新建 ELF 段对应的 VMA。
- **exit**：`do_exit` → `vma_free_all()` — 遍历链表释放所有 VMA + 物理页。

### 10. 16KB 簇策略

VMA 以 16KB 对齐分配空间（`vm_start`/`vm_end` 对齐到 16KB），但 `do_page_fault` 仅分配被触碰的 4KB 页。

```
VMA: [0x700000, 0x704000)  12KB 请求 → 对齐到 16KB
PTE:  0x700000 → 已映射 (触碰)
      0x701000 → 未映射      簇内未触碰
      0x702000 → 已映射 (触碰)
      0x703000 → 未映射      簇内共 4 个 4KB slot
```

`vma_find()` 不做额外簇逻辑——它只查 VMA 范围。`do_page_fault` 检查 `vm_start <= cr2 < vm_end`，触碰到的 4KB 页才分配。

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

## VFS 文件映射前置检查

文件映射按需加载路径需要 `vfs_lseek` 将文件偏移定位到 `vm_pgoff` 对应的位置，然后 `vfs_read` 读取 4KB。需要验证 VFS 层支持以下语义：

- `vfs_lseek(node, offset, SEEK_SET)` — 定位到绝对偏移（当前代码中未显式实现，需确认 `vfs_node_t` 是否有 `f_pos` 字段，或 `vfs_read` 接受显式 offset 参数）
- 如果 VFS 当前不支持 seek，文件映射路径可降级为：在 `do_page_fault` 时一次性 `vfs_read` 整个文件到临时 buffer（最大 2MB），然后按需映射到 PTE。这避免了 VFS 改造但浪费内存。

## 文件变更清单

| 文件 | 操作 | 内容 |
|------|------|------|
| `kernel/include/kernel/vma.h` | 新建 | `vma_t` 结构、`VM_*` 常量、`vma_find`/`vma_insert`/`vma_remove`/`vma_free_all` 声明 |
| `kernel/memory/vma.c` | 新建 | VMA 链表操作、`do_mmap`、`do_mprotect`、`do_munmap` |
| `kernel/memory/vmm.c` | 修改 | 新增 `vmm_map_4k_page`、`vmm_unmap_4k_page`、`vmm_pt_walk`、PAGE_USER_4K / PAGE_USER_4K_RO 宏 |
| `kernel/include/kernel/vmm.h` | 修改 | 新增 4KB 函数声明和 PTE 级标志宏 |
| `kernel/include/kernel/task.h` | 修改 | `mm_t` 新增 `vma_list`、`mmap_base` |
| `kernel/arch/x86_64/trap.c` | 修改 | `do_page_fault` 用户态路径重构（VMA 查找+按需分配+按需读文件）；`do_system_call` 新增 3 个 case（读取 regs->r8/r9/r10）；Linux ABI 表 nr 9/10/11 启用 |
| `kernel/sched/task.c` | 修改 | `do_fork` 调用 `fork_vma_copy`；`do_exit` 调用 `vma_free_all`；`sys_exec` 调用 `vma_free_all` |
| `kernel/memory/pmm.c` | 修改 | 新增 `alloc_4k_page()`、`free_4k_page()`（2MB pool + 内部 bitmap 管理 512 个 4KB 子页） |
| `kernel/include/kernel/pmm.h` | 修改 | 新增 `alloc_4k_page`/`free_4k_page` 声明 |
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
