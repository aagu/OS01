# 架构评审 — Group 2: 内存管理

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/memory/pmm.c`, `slab.c`, `vmm.c`, `vma.c`, `tlb.c`, `kernel/include/kernel/vmm.h`, `kernel/include/kernel/pmm.h`, `kernel/include/kernel/slab.h`, `kernel/include/kernel/vma.h`, `kernel/arch/x86_64/trap.c` (do_page_fault)

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P0 | vmm | `vmm_free_user_map` 只释放 2MB 大页，4KB PTE 页面全部泄漏 | 已修复 |
| 2 | P0 | vmm/exit | `do_exit` 显式跳过 `vmm_free_user_map`（COW fork 共享），2MB ELF 页 + 页表页全泄漏 | 已修复 |
| 3 | P1 | slab | `kmalloc`/`kfree` 无 SMP 锁，并发分配破坏 color_map | 待处理 |
| 4 | P1 | pmm | `alloc_pages` bitmap 扫描 O(n) 且使用复杂位运算而非 `__builtin_ctzll` | 待处理 |
| 5 | P1 | slab | `kmalloc_create` 递归依赖小 cache 预分配，invariant 脆弱 | 待处理 |
| 6 | P1 | slab | `kfree` XOR-based freelist 无双 free 检测 | 待处理 |
| 7 | P2 | vmm | `vmm_unmap_page` 返回 phys 而 `vmm_unmap_4k_page` 返回 void, API 不一致 | 待处理 |
| 8 | P2 | vma | VMA 列表操作（insert/remove/find）无锁，线程共享地址空间时会破坏 | 待处理 |
| 9 | P2 | tlb | TLB shootdown timeout 后无恢复机制 | 待处理 |
| 10 | P2 | pmm | PMM metadata 放置在 `_end` 后，布局线性依赖较脆弱 | 待处理 |
| 11 | P2 | pmm | `alloc_4k_page` 新 pool 路径中 slot 0 的 cow_count 未显式清零 | 待处理 |

---

### [P0] 1. `vmm_free_user_map` 只释放 2MB 大页，4KB PTE 页面全部泄漏

- **位置**: `kernel/memory/vmm.c:138-182`
- **现象**: 函数只处理 `PAGE_PS` 的 PML2E（2MB 大页）:
  ```c
  for (int l2 = 0; l2 < 512; l2++) {
      uint64_t pml2e = pml2[l2];
      if (!(pml2e & PAGE_Present)) continue;
      if (pml2e & PAGE_PS) {
          // ← 只在这里 free_pages
      }
      // ← 非 PS 的 4KB PTE entry 被静默跳过！
  }
  kfree(pml2);  // PDE table 被释放，但其内的 PTE 物理页未释放
  ```
  - 即使 `vmm_free_user_map` 被正确调用，它也会泄漏所有 4KB 物理页
  - PTE table 页本身虽被释放（`kfree(pml2)`），但指向的物理页全泄漏
- **建议**: 
  1. 在非 PS 分支中遍历 PTE table (512 entries)
  2. 对每个 present entry: COW 感知 (`page_cow_put` → `free_4k_page`) 释放
- **修复**: 在 PDE 遍历的 `else` 分支中，将 PTE table 指针转换为 `uint64_t *pt`，遍历 512 个 PTE 条目并释放。覆盖 `PAGE_COW`、`PAGE_PROTNONE`、普通 4KB 页三种情况。释放后 `kfree(pt)` 回收 PTE table 页本身。

  至此 `vmm_free_user_map` 完整释放用户地址空间所有物理页+页表页，P0#1 和 P0#2 均已修复。

### [P0] 2. `do_exit` 显式跳过 `vmm_free_user_map`，2MB 页 + 页表页全泄漏

- **位置**: `kernel/sched/task.c:455-464` (`do_exit`)
- **现象**:
  ```c
  vma_free_all(current->mm);  // 只释放 VMA 跟踪的 4KB 页

  if (!(current->flags & PF_KTHREAD) && current->mm) {
      // Skip vmm_free_user_map for now — in do_fork the child
      // shares pml4 with the parent, so freeing would corrupt
      // the parent's address space.  Will be fixed with mm refcounting.
      kfree(current->mm);       // 只 free mm_t struct
      current->mm = NULL;
  }
  ```
  - `vma_free_all` 只释放 VMA 管理的 4KB 页面（mmap/mprotect 分配的）
  - 2MB ELF 加载页（`spawn_user_task`/`sys_exec` 中 `alloc_pages` + `vmm_map_page`）**全部泄漏**
  - PML4 / PDPT / PDE / PTE 页表页 **全部泄漏**
  - `fork_mm_copy` 创建独立的子页表（`vmm_alloc_map` + `calloc` 每级），所以 `vmm_free_user_map(child_pml4)` 实际上是安全的——不会破坏父页表。注释的"shared pml4"是遗留理解，已过时
  - **注意**: 错误路径 `kfree(child_pml4)` (line 1196) 使用 `kfree` 是正确的——`vmm_alloc_map()` 返回 `calloc(1,4096)` → `malloc` → `kmalloc`，属 slab 分配，应用 `kfree` 释放
- **建议**:
  1. 修复 `vmm_free_user_map` 后（P0#1），在 `do_exit` 中恢复调用
  2. 或添加 mm refcount + `mm_users` 机制，最后一个引用时释放页表
- **修复**: `c14d2a3` — 在 `do_exit` 中恢复 `vmm_free_user_map`。修复前注释称 fork 共享 PML4 不能释放，实际 `fork_mm_copy` 创建独立页表层级（`vmm_alloc_map` + 逐级 calloc），所以释放是安全的。
  - 添加 `current->mm != parent->mm` 判断避免释放共享 mm（未来 PF_THREAD）
  - 还需 P0#1 修复 `vmm_free_user_map` 的 4KB PTE 页泄漏才能完全生效

### [P1] 3. Slab allocator (`kmalloc`/`kfree`) 无 SMP 锁

- **位置**: `kernel/memory/slab.c:117-181` (kmalloc), `183-252` (kfree)
- **现象**:
  - `kmalloc`: 操作 `color_map` bitmap (L168) 和修改 `total_free`/`total_using` (L172-173) 均无锁
  - `kfree`: 操作 `color_map` bitmap (L204) 和修改 cache 计数 (L209-210) 均无锁
  - 两个 CPU 同时 `kmalloc` 同大小对象 → 可能分配同一块内存 → 数据损坏
- **建议**: 给 `kmalloc_cache_size[i]` 加 per-cache spinlock，或在 slab 操作前后加全局 slab lock

### [P1] 4. `alloc_pages` bitmap 扫描使用复杂位运算而非 `__builtin_ctzll`

- **位置**: `kernel/memory/pmm.c:314-339`
- **现象**:
  ```c
  for (k = shift; k < 64; k++) {
      if (!((k ? ((*p >> k) | (*(p + 1) << (64 - k))) : *p) & (num))) {
          // found free run starting at bit k
  ```
  - 逐 bit 扫描 O(n)，对大内存（数百 GB）非常慢
  - `alloc_4k_page` 已使用 `__builtin_ctzll`（更高效），但 `alloc_pages` 未同步更新
- **建议**: 将 `alloc_pages` bitmap 扫描改为 `__builtin_ctzll` + 连续位检查

### [P1] 5. `kmalloc_create` 递归依赖小 cache 预分配

- **位置**: `kernel/memory/slab.c:87` (kmalloc_create)
- **现象**:
  - Cache 8-15 (1024B–1MB) 的 `kmalloc_create` 调用 `kmalloc(sizeof(struct Slab))` (L87) 和 `kmalloc(slab->color_length)` (L95)
  - `kmalloc` 若缓存为空则调用 `kmalloc_create` → 递归。仅因 `sizeof(struct Slab)` (~88B) 落在预分配的 caches 0-7 中才避免死循环
  - 若改动 `sizeof(struct Slab)` 或 cache 边界，递归栈会碎裂
- **建议**: 添加递归守卫或为 `struct Slab` 分配使用专用 cache

### [P1] 6. `kfree` XOR 双 free 破坏 allocator

- **位置**: `kernel/memory/slab.c:202-204` (注释已说明)
- **现象**:
  ```c
  // XOR toggles the bit back to 0 (free).
  // NOTE: this is NOT idempotent — double-free would re-mark
  // the block as allocated, corrupting the allocator.
  *(slab->color_map + (index >> 6)) ^= 1UL << (index % 64);
  ```
  - XOR 操作：第一次 free 将 bit 从 1→0（正确释放）。第二次 free 从 0→1（错误地标记为已分配）
  - 后续 `kmalloc` 会分配这个"假空闲"的 block，导致两个指针指向同一内存
- **建议**: 改为 AND-NOT（`&= ~bit`）+ 在释放前检查 bit 是否为 1（检测双 free）：
  ```c
  if (!(slab->color_map[word] & bit)) { /* double free error */ }
  slab->color_map[word] &= ~bit;
  ```

### [P2] 7. `vmm_unmap_page` 与 `vmm_unmap_4k_page` API 不一致

- **位置**: `kernel/memory/vmm.c:53-73` vs `256-277`
- **现象**:
  - `vmm_unmap_page` (2MB): 返回物理地址，调用者负责释放
  - `vmm_unmap_4k_page` (4KB): 返回 void，内部调用 `free_4k_page`
  - 调用者容易混淆使用哪个、谁负责释放
- **建议**: 统一 API 命名或添加注释说明使用场景

### [P2] 8. VMA 列表操作无锁

- **位置**: `kernel/memory/vma.c:13-109` (vma_find/insert/remove/free_all)
- **现象**: 所有 VMA 操作直接在 `current->mm->vma_list` 上操作，无任何锁保护
  - 当前无用户态线程（PF_THREAD 存在但 `mm` 是 per-task），所以无竞争
  - 但若未来实现 clone(CLONE_VM) 共享地址空间，`mmap`/`munmap` 并发调用会破坏链表
- **建议**: 在 `mm_t` 中添加 VMA spinlock（`mm->vma_lock`），在 do_mmap/do_munmap/do_mprotect 中持有

### [P2] 9. TLB shootdown timeout 后无恢复

- **位置**: `kernel/memory/tlb.c:45-59`
- **现象**:
  ```c
  uint32_t timeout = 100000;
  while (percpu_data[i].tlb_ack == 0 && --timeout > 0)
      arch_cpu_pause();
  if (timeout == 0) {
      debug_mm("TLB shootdown: CPU %u did not ACK\n", i);
  }
  // 即使 timeout 也重置 flags:
  percpu_data[i].tlb_ack = 0;
  percpu_data[i].tlb_wanted = 0;
  ```
  - 超时后没有 invalidation 保证，发起 CPU 可能有 stale TLB
- **建议**: 超时后至少执行 `flush_tlb()` 作为 fallback

### [P2] 10. PMM metadata 线性布局脆弱

- **位置**: `kernel/memory/pmm.c:141-157`
- **现象**: `bits_map`、`pages_struct`、`zones_struct` 均放置在 `_end` 之后，通过 `PMMngr.start_brk` 计算对齐地址。所有元数据大小取决于 E820 entries（总 RAM 大小），无保留区域
- **建议**: 无紧迫修复需求。未来可考虑在链接脚本中保留固定大小的 PMM region，或使用动态分配

### [P2] 11. `alloc_4k_page` 新 pool 的 slot 0 cow_count 未清零

- **位置**: `kernel/memory/pmm.c:474-489`
- **现象**: 新分配的 subpage_pool 中：
  - `memset(pool->bitmap, 0, 64)` 只清零 bitmap (64 bytes)
  - `pool->cow_count[512]` 是 uint16_t[512] = 1024 bytes，未被清零（2MB page 来自 alloc_pages，内容不确定）
  - slot 0 的 cow_count 随机，但 slot 0 是 subpage_pool struct 本身，不应被 `page_cow_get/put` 引用
- **建议**: 添加 `memset(pool->cow_count, 0, sizeof(pool->cow_count))` 或确保 pool 使用 `calloc`
