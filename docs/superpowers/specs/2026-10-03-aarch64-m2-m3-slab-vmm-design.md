---
title: OS01 aarch64 M2 Slab + M3 运行期 VMM 设计 (v2)
created: 2026-10-03
type: spec
status: revised-for-review
tags: [osdev, aarch64, memory, slab, vmm, tlb, ipi]
supersedes: 93f8ae7 (v1)
---

# aarch64 M2 Slab + M3 运行期 VMM (v2)

> v2 是 v1（commit `93f8ae7`）的用户评审修订版。14 项问题中 7 项阻塞级（1, 2, 3, 4, 5, 6, 7）+ 5 项重要（8, 9, 10, 11, 12）+ 2 项一般（13, 14）全部修复。本节先列 v2 关键修订摘要，再展开全文。

## 0. v2 关键修订摘要

| 项 | v1 错误 | v2 修正 |
|----|----|----|
| 1 | 假定「M1 → Slab」启动顺序 | 实测 `aarch64/boot/main.c:411` → `pmm.c:317` → `aarch64/boot/main.c:422`：顺序是 **PMM → Slab → M1**。真实 Slab 在 M0 直映（0..2 GiB TTBR0）下运行，必须确保 Slab 元数据与 8 个预分配 2 MiB 页落在 M0 覆盖范围内。 |
| 2 | 假定 PMM 元数据就绪 = Slab 可用 | 实测 `slab_init` 在 `end_of_struct` 后写缓存元数据，并占用下一处 2 MiB 边界的 8 个预分配页。**这些 frame 必须由 M1 arena 显式预留**，否则会与 M1 表页池冲突或占用未保留 RAM。 |
| 3 | 假定 `tlb_shootdown()` 与 `IPI_VECTOR_TLB` 可直接复用 | 实测 `kernel/memory/tlb.c` **未编入 aarch64 build**；`ipi_broadcast` 只有 x86 APIC 实现；`IPI_VECTOR_TLB = 0x40` 是 x86 vector 编号，不能直接当作 GICv2 SGI。M3 必须包含 aarch64 SGI 发送 + handler 注册 + `tlb_shootdown()` 编译接入，作为独立前置任务。 |
| 4 | split 直接覆盖 block 描述符 | ARM ARM 要求 **break-before-make**：先使旧项无效 → dsb ishst → local tlbi → dsb ish → 跨核 tlb_shootdown → 写新项 → dsb ishst → local tlbi → dsb ish + isb。SMP 下需要 per-page-table 锁；当前 `aarch64/page_table.h:10` 限制活跃 root 仅 BSP pre-SMP 修改，M3 须明确解除该限制所需的同步方案。 |
| 5 | 假定 vmm.h flag 重命名 + 旧 wrapper 可让 caller 跨架构 | 实测 `vma.c:819-849`、`uaccess.c:149/203/220-221`、`task.c:2016-2082` 直接读取、组合、写回原始 PTE，遍历 x86 格式上级描述符。**M3 不编入这些模块到 aarch64**（vma/uaccess 整体不进；task.c 仅 fork/clone 段不进），caller 范围严格限定。 |
| 6 | `decode_perm` 补 bit 55/56 即可 round-trip PROT_NONE | 实测 `aarch64_pt_query_4k` 遇 `Valid=0` 直接返回 `ENOENT`；PROT_NONE 必须 `Valid=0` 但保留 PA。**需定义新返回码（如 `EPROT_NONE`）**，并让 `query_4k` / `unmap_4k` / `free_user_map` 处理"V=0 + bit 55"状态。 |
| 7 | `arch_vmm_free_user_map` 遍历 L0[0..255] | aarch64 TTBR0 root 用完整 L0[0..511]。**`arch_vmm_free_user_map` 推迟到 M4**，M3 不实现。 |
| 8 | `arch_vmm_map_4k` 单一接口 | 拆为 `arch_vmm_map_4k_new`（EEXIST 遇有效 PTE，不覆盖）+ `arch_vmm_update_4k`（替换，明确旧 PA 处理）。x86 wrapper 保持覆盖语义走 `arch_vmm_update_4k`。 |
| 9 | `arch_vmm_unmap_2m` 缺契约 | 补齐：pmd 必须是 block；返回原 PA（bits [39:12]）；不释放物理页（caller 决定）；local TLBI + shootdown；L2 表保留。 |
| 10 | R6 要求所有 backend 用 `alloc_4k_page` | 改为 per-backend 自决：x86 沿用 calloc/kfree；aarch64 用 alloc_4k_page/free_4k_page。 |
| 11 | 软件位走 `encode_perm` 扩展 | `encode_perm` 当前拒绝未知位。**新增独立软件位参数**：扩展 `aarch64_pt_map_4k` 接受 `software_bits`（或把 VM_PROTNONE/VM_COW 纳入 perm 集合作为新软件位常量 `AARCH64_PT_SOFTWARE_*`）。 |
| 12 | 验收要求 shell 提示符 + `aarch64 systest_repeat.py` | 当前 aarch64 boot 以 halt 收尾且无用户态。**M2/M3 验收改为 `KERNEL_SELFTEST=1` QEMU 启动到 main + selftest 全过**。x86 systest_repeat 维持。aarch64 systest 推迟到 M4+。 |
| 13 | aarch64 `arch_flush_tlb_all` 假定为 weak default；指向不存在的 `kernel/arch/aarch64/Makefile` | 实测 `kernel/include/arch/mmu.h:144` 已有内联 `tlbi vmalle1 + dsb sy + isb` 实现；source gate 在 `kernel/Makefile:39` 一带（具体行号以 build profile 实际行号为准）。 |
| 14 | 测试矩阵 8 种组合、`test_vmm_split_merge` 命名 | 修正为 **12 种合法组合**（USER/KERNEL × RO/RW × EXEC/NOEXEC × NORMAL/DEVICE，扣除 DEVICE\|EXEC）+ **4 种拒绝组合**（DEVICE\|EXEC × {RO,RW} × {USER,KERNEL} = 4 个 case）。`test_vmm_split_merge.c` 改名 `test_vmm_split.c`，仅测 split。 |

---

## 1. 意图、基线与完成标准

依据 roadmap P2，M2 让 aarch64 kernel 从 `kmalloc()=NULL` 走到能在任意缓存大小上分配/释放；M3 让运行期既可映射/解除映射 4 KiB 也可创建/拆分 2 MiB block，并保留 x86_64 全部现行行为。M2+M3 是 M4 用户地址空间的前置；M4 不在本 spec。

**基线**：master @ `dabc9f0`。前置依赖 M0 ✅ / M1 ✅ / Generic Timer ✅ / GICv2 Phase 1 ✅ / IPI fix ✅ / PMM arch-neutral ✅，均已 master。

### 1.1 aarch64 启动顺序实测（v2 修正）

`kernel/arch/aarch64/boot/main.c` 实测启动序列（行号以当前代码为准）：

1. `arch_cpu_early_init()`：CPU 特性探测、栈/CPUs 准备。
2. `aarch64_ram_init()` + `aarch64_early_arena_init()`（约 line 388-405）：M1 preflight、arena 选择与发布。
3. **`pmm_init(handoff)`**（line 411）：**进入 PMM 初始化**。
   - `pmm_init` 内部（`kernel/memory/pmm.c:317`）调 **`slab_init()`**——这是真实 Slab 被调用的最早时机。
4. `arch_boot_direct_map_init()`（line 422）：M1 运行期直映**在 Slab 之后**建立。

**结论**：M2 实施时真实 Slab **在 M1 直映建立之前**运行，运行在 M0 直映（TTBR0 0..2 GiB）下。这意味着：

- Slab 的所有元数据 + 8 个预分配 2 MiB 页 **必须落在 M0 直映覆盖范围 [0, 2 GiB) 内**；
- M1 arena 选择时 **必须把 Slab 占用的 frame 显式预留**，否则 PMM 会把它们当普通 RAM 分配掉。

### 1.2 M2 完成标准（v2 修正）

- aarch64 build 编入 `kernel/memory/slab.c`，移除 `kernel/arch/aarch64/runtime/slab_stub.c`。
- `slab.c` 的 `slab_lock_acquire/release` 改用 `arch_local_irq_save/restore`（现有 facade，`kernel/include/arch/irq.h`）；`slab.c` 内不再出现 `pushfq` / `cli` / `sti` / `popfq` 字面。
- **M1 arena 扩展**：在 `aarch64_early_arena_init` 计算 arena 大小时，把真实 Slab 的需求（缓存元数据大小 + 8 × 2 MiB 预分配页）显式纳入预留范围并加入 boot reservation；启动日志输出预留量。
- **边界检查**：Slab 的 `end_of_struct` 地址与 8 个预分配页起始地址必须在 M0 直映 [0, 2 GiB) 内；超出范围则 FATAL 早停（不进入 `pmm_init`）。
- aarch64 kernel `KERNEL_SELFTEST=1` 启动到 main 不 panic；`slab_init` 返回非 0；selftest 跑跨 16 缓存大小 kmalloc/kfree pass。
- x86_64 `test/hosttest/memory/` 全部 + `systest_repeat.py` 5 连 pass。

### 1.3 M3 完成标准（v2 修正）

- **`kernel/memory/tlb.c` 接入 aarch64 build**：aarch64 编译单元包含 `tlb_shootdown()`；其内部 `flush_tlb()` 走 `arch_flush_tlb_all()`（`kernel/include/arch/mmu.h:144` 已实现 `tlbi vmalle1 + dsb sy + isb`，无需新增内联）。
- **aarch64 IPI 发送**：GICv2 SGI 编号映射（`IPI_VECTOR_TLB=0x40` 翻译为某 SGI ID，例如 SGIn=0）+ `arch_ipi_broadcast()` 实现；aarch64 `ipi_broadcast()` 经此发送。
- **aarch64 TLB handler 注册**：在 `IPI_VECTOR_TLB` 对应 SGI 上注册 handler，跑 `arch_flush_tlb_all()` + ack 协议。
- **`kernel/include/memory/vmm.h`** 暴露 arch-neutral 语义 flag（`VM_PRESENT` / `VM_WRITE` / `VM_USER` / `VM_NO_EXEC` / `VM_HUGE` / `VM_NOCACHE` / `VM_PROTNONE` / `VM_COW`），位位置不再外泄。
- **`kernel/include/arch/<arch>/vmm_backend.h`** + **`kernel/arch/<arch>/memory/vmm_backend.c`** per-arch 实现：
  - `arch_vmm_init`、`arch_vmm_map_4k_new`、`arch_vmm_update_4k`、`arch_vmm_unmap_4k`、`arch_vmm_map_2m`、`arch_vmm_unmap_2m`、`arch_vmm_split_2m_to_4k`、`arch_vmm_pt_walk`。
  - **`arch_vmm_free_user_map` 推迟到 M4**（M3 不实现）。
- **aarch64 `aarch64_pt_*` 扩展**：
  - 新增 `aarch64_pt_map_2m_block` / `aarch64_pt_unmap_2m_block`；
  - 新增 `aarch64_pt_split_block_2m`；
  - 扩展 `aarch64_pt_map_4k` / `aarch64_pt_query_4k` / `aarch64_pt_unmap_4k` 支持独立软件位参数 `software_bits`（见 §5.4），并新增返回值 `AARCH64_PT_EPROT_NONE`（V=0 + bit 55）。
- **break-before-make 协议**：活跃 root 上 block ↔ table 替换走 §5.3 完整顺序；非活跃 root 维持 BSP-only 修改。
- **caller 范围限制**：
  - `kernel/memory/vma.c` **不编入 aarch64**；
  - `kernel/memory/uaccess.c` **不编入 aarch64**；
  - `kernel/sched/task.c` 中 fork/clone 的 PTE 拷贝段（约 line 1988-2082）**不编入 aarch64**；其余非 PTE 操作段保留；
  - 后续 M4 引入 aarch64 兼容的 vma/uaccess/fork 接口时再恢复编入。
- **aarch64 `arch_vmm_init` 把 M1 root 直接注册为 `kernel_map`**。
- **host 边界测试**（12 合法权限组合 + 4 拒绝组合 + split 内容 + 软件位 round-trip，见 §8）。
- **QEMU aarch64 `KERNEL_SELFTEST=1`** 启动到 main + selftest 全过 + 多核 TLB shootdown 自测通过。
- x86_64 `systest_repeat.py` 5 连 pass。

### 1.4 不属于本 spec（明确边界）

- M4 用户 PGD / EL0 切换 / uaccess 故障恢复（roadmap 已列）；
- `arch_vmm_free_user_map` 实现（M3 推迟到 M4）；
- `merge_4k_to_2m`（F1）；
- `per-VA TLB shootdown`（F2）；
- `vma.c` / `uaccess.c` 在 aarch64 的重写（M4 范围）；
- Slab 算法优化（per-CPU cache / NUMA / page coloring）；
- ASLR / 用户栈随机化。

---

## 2. 已核实的约束（v2 修正）

1. **`slab.c:36-55`** `slab_lock_acquire` 用 inline `pushfq; cli`，`slab_lock_release` 用 `sti`。`arch_local_irq_save/restore` 已存在（`kernel/include/arch/irq.h:55`），x86 用 pushfq+cli，aarch64 用 `mrs daif` + `msr daifset, #2`。可直接替换，无 API 变化。

2. **`slab_stub.c`** 是 4 个空函数（`slab_init` / `kmalloc` / `kfree` / `kzalloc` / `ksize`），签名与 `kernel/memory/slab.c` 完全一致。

3. **aarch64 启动顺序实测**：`aarch64/boot/main.c:411 pmm_init()` → `pmm.c:317 slab_init()` → `aarch64/boot/main.c:422 arch_boot_direct_map_init()`。Slab 在 M1 之前运行。

4. **M0 直映范围**：`aarch64/head.S` 在 MMU 打开前建 0..2 GiB boot map（Normal/EL1 RW）；M1 在 [0x40000000, 0x40200000) 保留内核 block，其余 RAM 通过 M1 arena 走正常 RAM 直映。

5. **`slab.c` 真实占用**：`slab_init` 在 `end_of_struct` 之后写缓存元数据（`kmalloc_cache_size[16]` + `slab_lock` + `slab_lock_depth[]`），并从下一处 2 MiB 边界占 8 个预分配页（按 `kmalloc_create` 内 `alloc_pages(ZONE_NORMAL,1,0)` 计）。这些 frame **必须**：
   - 物理地址在 [0, 2 GiB) 内（M0 直映可达）；
   - **不**与 M1 arena 重叠；
   - **不**被 PMM 当普通 RAM 分配；
   - 由 boot reservation 标记为 Kernel/Init。

6. **`kernel/memory/vmm.c:115-117`** `vmm_init()` 把 `kernel_map = (uint64_t *)Phy_To_Virt(0x101000)`——x86_64 启动期 PML4 物理地址硬编码。aarch64 端无等价物；M1 root 必须从 `aarch64_read_ttbr1()` 拿。`vmm_init` 必须去 x86-ize。

7. **`vmm.c:69-90` `vmm_map_page`** 假设 L2 永远是 2 MiB block；`vmm_pt_walk:289-291` 拒绝 `pmd[l2] & PAGE_HUGE`。M3 split 是 hook 点。

8. **`vmm.h:33-39` 注释自承**位位置 x86 专属；这是 v2 flag 重命名的依据。

9. **`aarch64_pt_map_4k:430`** 遇有效 PTE 返回 `EEXIST`；与 x86 `vmm_map_4k_page:313` 直接覆盖语义不一致。需拆为 `map_new` / `update` 两个接口（见 §4.4）。

10. **`aarch64_pt_query_4k:454`** 遇 `Valid=0` 直接返回 `ENOENT`；PROT_NONE（Valid=0 + bit 55）无法 query 现有 PA。需新增 `AARCH64_PT_EPROT_NONE` 返回值 + `query_4k` 在 `V=0 + PROTNONE` 时返回此码。

11. **`aarch64/encode_perm:240`** 拒绝 `AARCH64_PT_PERM_ALL_BITS` 之外的位（VM_PROTNONE/VM_COW 在此集合外）；`aarch64_pt_map_4k` 只接受此函数输出。需扩展原语支持独立软件位（见 §5.4）。

12. **`arch_flush_tlb_all()` aarch64 实现已存在**：`kernel/include/arch/mmu.h:144-148` 提供 `tlbi vmalle1 + dsb sy + isb`。M3 不需新增 inline。

13. **`tlb_shootdown()` 当前 x86-only**：`kernel/memory/tlb.c` 未编入 aarch64 build（待 `kernel/Makefile` 配置确认具体行号；v1 假设已编入是错的）。M3 须包含编译接入。

14. **`ipi_broadcast()` 当前 x86-only**：`kernel/arch/x86_64/intr/` 下有 APIC 实现；aarch64 端当前仅有 `aarch64-ipi-fail-handoff-2026-09-26.md` 修复后的 GIC SGI 收发，但需新增 `arch_ipi_broadcast()` 通用入口或 aarch64-specific 函数供 `tlb_shootdown` 使用。`IPI_VECTOR_TLB=0x40` 是 x86 vector 编号，GICv2 SGI 编号 0..15，需做映射。

15. **直接操作 PTE 的 caller**（v2 新发现）：
   - `kernel/memory/vma.c:150,173,350,420,821-849` 直接读写 `PAGE_USER`/`PAGE_WRITE`/`PAGE_VALID`/`PAGE_PROTNONE`/`PAGE_COW`，构造 x86 格式 PTE；
   - `kernel/memory/uaccess.c:149,203,220-221` 操作 `PAGE_COW`；
   - `kernel/sched/task.c:1988-2082` fork 路径遍历 x86 格式 PGD/PUD/PMD/PTE 并复制；
   - 这些代码段在 M3 **不编入 aarch64**。

16. **`aarch64/page_table.h:10`** 注释限制活跃 root 仅 BSP pre-SMP 修改；M3 须明确解除该限制所需的锁/同步方案（见 §5.3）。

---

## 3. M2 设计

### 3.1 改动清单

1. **`kernel/memory/slab.c`**：
   - 顶部增加 `#include <arch/irq.h>`。
   - `slab_lock_acquire`：返回类型由 `uint64_t` 改为 `arch_irq_state_t`；函数体第一句替换为 `arch_irq_state_t flags = arch_local_irq_save();`；末尾 `return flags;`。
   - `slab_lock_release`：形参类型 `uint64_t flags` → `arch_irq_state_t flags`；函数体末尾 `if (flags & (1UL << 9)) __asm__ __volatile__("sti" ::: "memory");` → `arch_local_irq_restore(flags);`。
2. **`kernel/arch/aarch64/runtime/slab_stub.c`**：删除整个文件。
3. **aarch64 build profile**（`kernel/Makefile` 与 `kernel/arch/aarch64/` 下等价位置，参照 `docs/aarch64-libk-aarch64-closure-2026-09-24.md` 中 PMM/libk 接入的同款 source group 动作）：把 `kernel/memory/slab.c` 加入 aarch64 `kernel-y`；`slab_stub.c` 退出 aarch64 编译源。
4. **`kernel/include/arch/irq.h` / per-arch 实现**：不动。

### 3.2 M1 arena 扩展（v2 新增）

**目标**：在 `aarch64_early_arena_init`（`kernel/arch/aarch64/memory/early_arena.c`）计算 arena 大小与 boot reservation 时，把真实 Slab 的需求显式纳入。

**改动**：

1. 在 `aarch64_early_arena_init` 内调 `slab_estimate_size()`（新增 helper，见下）拿到：
   - `slab_metadata_bytes`：缓存元数据总大小（`kmalloc_cache_size[16]` + `slab_lock` + `slab_lock_depth[NR_CPUS]` + 内部 metadata）；
   - `slab_reserved_2m_pages`：8（与 `slab.c::kmalloc_create` 中 `alloc_pages(ZONE_NORMAL,1,0)` 调用次数一致——M2 实施时需精确核算，v2 给上界）。
2. 把 `slab_metadata_bytes` 向上对齐到 2 MiB 计入 arena 元数据段。
3. 把 `slab_reserved_2m_pages × 2 MiB` 计入 arena 后的预留范围，作为 boot reservation 注册到 PMM（属性 Kernel/Init，不进 `alloc_pages` 普通分配池）。
4. 启动日志输出预留量；`arch_boot_direct_map_init` 验证这些 frame 在 M0 直映 [0, 2 GiB) 内，否则 FATAL。

**`slab_estimate_size()` 定义**（新增于 `kernel/memory/slab.c` 或 `kernel/include/memory/slab.h`）：

```c
/* 返回 slab 初始化所需的元数据字节数与预分配 2 MiB 页数。
 * 由 aarch64 启动路径在 slab_init() 之前调；返回值为静态上界，
 * 用于 M1 arena 预留。slab_init() 实际使用量 ≤ 返回值。 */
struct slab_estimate {
    size_t metadata_bytes;
    size_t reserved_2m_pages;
};
struct slab_estimate slab_estimate_size(void);
```

实现：`metadata_bytes = sizeof(kmalloc_cache_size) + sizeof(slab_lock) + sizeof(slab_lock_depth) + sizeof(struct Slab) * 16`（上界）；`reserved_2m_pages = 8`。

### 3.3 初始化顺序验证

按实测顺序：**PMM 元数据 → M0 直映已就绪 → `slab_init()` 在 `pmm_init` 内调用 → M1 直映**。M2 接受此顺序；aarch64 启动日志增加 `slab_init: ok size=<bytes>` 作为隐式验证（slab 失败会 early printk 并 halt）。

### 3.4 M2 不动项

- 不改 slab 算法本身（per-CPU cache / NUMA / page coloring）；
- 不动 `kmalloc_create` 内 case 分支（只动锁）；
- 不优化 `kmalloc_creating` 递归 flag（F4）。

---

## 4. vmm.h 抽象与 caller 迁移策略（v2 修正）

### 4.1 语义 flag 命名（caller 视角）

同 v1 表。

### 4.2 接口（arch-neutral，v2 修正）

`kernel/include/memory/vmm.h`：

```c
#include <arch/vmm_backend.h>   /* per-arch VM_* 定义 + arch_vmm_* 声明 */

#define VM_KERNEL_RW    (VM_PRESENT | VM_WRITE)
#define VM_KERNEL_RO    (VM_PRESENT)
#define VM_USER_RW      (VM_PRESENT | VM_WRITE | VM_USER)
#define VM_USER_RO      (VM_PRESENT | VM_USER)
#define VM_DEVICE       (VM_PRESENT | VM_WRITE | VM_NOCACHE)

extern uint64_t *kernel_map;

int   arch_vmm_init(void);
int   arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags,
                          uint64_t *old_phys_out);
int   arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);
uint64_t *arch_vmm_pt_walk(uint64_t *pgdir, uint64_t virt, uint32_t vm_flags, int allocate);
/* arch_vmm_free_user_map 推迟到 M4。 */

/* 旧符号保留 wrapper，调用对应 arch_vmm_*，仅 x86_64 build 链接。 */
void      vmm_map_page(uint64_t *pgdir, uintptr_t pa, uintptr_t va, uint64_t flags);
uintptr_t vmm_unmap_page(uint64_t *pgdir, uintptr_t va);
mmap      vmm_alloc_map(void);
```

旧 `PAGE_*` 名字不再保留 alias——所有 caller 一次性迁到 `VM_*`，避免误导 M4 caller 直接使用 x86 PTE bit（见 §4.5）。

### 4.3 软件位处理（v2 修正）

VM_PROTNONE / VM_COW 是软件位，位位置随 arch 而变：
- x86：bit 9 / bit 10（PTE 软件位，硬件忽略）；
- aarch64：bit 55 / bit 56（descriptor 保留区，硬件忽略）。

**传递方式**（v2 新规）：

```c
/* arch/<arch>/vmm_backend.h */
#define VM_PROTNONE_BIT    (1UL << 0)   /* 软件位槽位 0：x86=bit 9, aarch64=bit 55 */
#define VM_COW_BIT         (1UL << 1)   /* 软件位槽位 1：x86=bit 10, aarch64=bit 56 */

typedef uint32_t vm_software_t;   /* 软件位掩码，使用 VM_PROTNONE_BIT / VM_COW_BIT */

/* 公共 API 增加 software 参数 */
int arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                         uint32_t vm_flags, vm_software_t sw);
int arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                        uint32_t vm_flags, vm_software_t sw,
                        uint64_t *old_phys_out);
int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                       vm_software_t *sw_out);
```

软件位翻译在 `arch/<arch>/memory/vmm_backend.c::vm_to_software_bits()` 内做：
- x86：`VM_PROTNONE_BIT → (1 << 9)`，`VM_COW_BIT → (1 << 10)`；
- aarch64：`VM_PROTNONE_BIT → (1 << 55)`，`VM_COW_BIT → (1 << 56)`。

### 4.4 per-arch backend 行为契约（v2 修正）

#### 4.4.1 aarch64 vmm_backend

1. `arch_vmm_init()`：从 `aarch64_read_ttbr1()` 读 M1 直映根，赋给 `kernel_map`；返回 0（见 §7）。
2. `arch_vmm_map_4k_new()`：
   - 若目标 L2 是 2 MiB block：先 `arch_vmm_split_2m_to_4k(virt)`；
   - 调 `aarch64_pt_map_4k(root, va, pa, vm_to_perm(vm_flags), vm_to_sw(sw))`（**`aarch64_pt_map_4k` 已扩展 software_bits 参数，见 §5.4**）；
   - 遇有效 PTE：返回 `EEXIST`，**不覆盖**；
   - 成功路径：`aarch64_pt_invalidate_local(va)` + `tlb_shootdown()`。
3. `arch_vmm_update_4k()`：
   - 走 `walk_to_l3`（create=false）；
   - 若目标有效：保留旧 PA → `*old_phys_out`；读旧软件位（仅 backend 内部使用，不外传）；
   - 写新描述符：`(phys & PA_MASK) | vm_to_perm(vm_flags) | vm_to_sw(sw)`；
   - `dsb_ishst; aarch64_pt_invalidate_local(va); tlb_shootdown()`；
   - **调用方负责释放 `*old_phys_out`（或保留，看语义）**。
4. `arch_vmm_unmap_4k()`：
   - 走 `walk_to_l3`；
   - 若 `V=0 + PROTNONE`：返回 `*phys_out = desc & PA_MASK`；清条目；`free_4k_page(*phys_out)`；`tlb_shootdown`；
   - 若 `V=0` 且无 PROTNONE：返回 `ENOENT`；
   - 若 `V=1`：返回 `*phys_out = desc & PA_MASK`；读旧软件位（仅 backend 内部使用，不外传）；清条目；`tlb_shootdown`。
5. `arch_vmm_map_2m()`：
   - 校验 `va & (2 MiB - 1) == 0`、`pa & (2 MiB - 1) == 0`、`pa < 1 TiB`；
   - `ensure_child_table` 拿 `pmd`；若 `pmd[l2]` 已占用（V=1）返回 `EEXIST`；
   - 写 block 描述符；
   - `aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()`。
6. `arch_vmm_unmap_2m()`：
   - `pmd[l2]` 必须是 block（V=1, bit 1 = 0）；否则 `EINVAL`；
   - 返回 `*phys_out = pmd[l2] & BLOCK_PA_MASK`（即 bits [39:12]）；
   - **不释放物理页**——caller 决定 `free_pages(Phy_to_2M_Page(phys), 1)`；
   - 清条目；
   - `aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()`；
   - **L2 表页保留**——结构仍在，仅条目清除。
7. `arch_vmm_split_2m_to_4k(virt)`：见 §5.3 break-before-make 协议。
8. `arch_vmm_pt_walk()`：若 L2 是 block 则返回 NULL（caller 必须先 split）；同 `vmm_pt_walk:289-291` 语义。

**注**：aarch64 backend 所有"分配表页"操作走 `alloc_4k_page()`（PMM 路径，不依赖 Slab）。

#### 4.4.2 x86_64 vmm_backend

从现有 `kernel/memory/vmm.c` 搬实现，按 `VM_*` 翻译原 `PAGE_*` flag。**关键差异**：

1. **保持 calloc/kfree**：x86 backend 的表页仍由 calloc 分配、kfree 释放，不强制迁到 `alloc_4k_page`/`free_4k_page`（避免引入无关的 slab/PMM 依赖耦合）。
2. **保持覆盖语义**：原 `vmm_map_4k_page` 直接覆盖 PTE；x86 backend 把旧 wrapper 路由到 `arch_vmm_update_4k`，对应 caller 拿回旧 PA（如 caller 不需要就忽略）。
3. **`vmm_init` 行为不变**：PMM walk + kernel_map = `Phy_To_Virt(0x101000)` + `tlb_shootdown`。

### 4.5 caller 迁移策略（v2 新增）

**M3 不编入 aarch64 的模块**：

| 模块 | 行号范围 | 处理 |
|----|----|----|
| `kernel/memory/vma.c` | 全文 | aarch64 build 排除；M4 重写 |
| `kernel/memory/uaccess.c` | 全文 | aarch64 build 排除；M4 重写 |
| `kernel/sched/task.c` | 约 line 1988-2082（fork PTE 拷贝） | aarch64 build 排除该段；M4 重写 |

**实施方式**：在 `kernel/Makefile` 或 source group 入口用 `ifneq ($(ARCH),aarch64)` 排除；M3 完成后跑 `aarch64 build` 必须不引用这些文件。

**旧 PAGE_* alias 不保留**：避免误导后续 caller 直接使用 x86 PTE bit。caller 全部改用 `VM_*`。

### 4.6 软件位与原语扩展（v2 修正）

`aarch64_pt_map_4k` 当前签名 `int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa, uint32_t perm)`，`encode_perm` 拒绝 VM_PROTNONE/VM_COW。

**v2 扩展方案**：

```c
/* kernel/include/arch/aarch64/page_table.h 新增 */
#define AARCH64_PT_SOFTWARE_PROTNONE  UINT64_C(0x0080000000000000)  /* bit 55 */
#define AARCH64_PT_SOFTWARE_COW       UINT64_C(0x0100000000000000)  /* bit 56 */

int aarch64_pt_map_4k(uint64_t *root, uint64_t va, uint64_t pa,
                       uint32_t perm, uint64_t software_bits);
int aarch64_pt_query_4k(const uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out,
                         uint64_t *software_bits_out);
int aarch64_pt_unmap_4k(uint64_t *root, uint64_t va,
                         uint64_t *pa_out, uint32_t *perm_out,
                         uint64_t *software_bits_out);
/* 新增返回值 */
#define AARCH64_PT_EPROT_NONE  (-6)   /* V=0, PROTNONE set, PA valid */
```

`encode_perm` 接受 software_bits 作为额外参数（不入 perm 集合），由 caller 显式传入。`decode_perm` 输出 software_bits 给 caller。`walk_to_l3` 不感知软件位；PTE 写入由 caller 组合。

---

## 5. aarch64 block + split（v2 修正）

### 5.1 block 描述符编码

同 v1 §5.1。

### 5.2 `aarch64_pt_map_2m_block` / `unmap_2m_block`（v2 新增）

**map**：

- 校验：`root_valid` / `va_canonical` / `va & (2 MiB - 1) == 0` / `pa & (2 MiB - 1) == 0` / `pa < 1 TiB`；
- `ensure_child_table` 走 L0/L1 拿 `pmd`；
- 若 `pmd[l2]` 已占用：返回 `EEXIST`；
- `encode_block_desc` 生成描述符；
- `dsb_ishst(); pmd[l2] = desc; dsb_ishst();`
- `aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()`。

**unmap**：

- `pmd[l2]` 必须是 block（V=1, bit 1 = 0）；否则 `EINVAL`；
- `*pa_out = pmd[l2] & BLOCK_PA_MASK`；
- **不释放物理页**（caller 决定）；
- 清 `pmd[l2]`；
- `aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()`。

### 5.3 break-before-make 协议（v2 新增，活跃 root block ↔ table 替换）

活跃 root（即 `is_active_root(root) == true`）上做 block 描述符 → table 描述符替换时，必须遵循 ARM ARM 的 break-before-make 顺序：

```
lock(pt_lock_for(root, va));        /* per-(root, l0_idx, l1_idx) spinlock */
1. pmd[l2] = 0;                       /* 使旧 block 描述符无效 */
2. dsb ishst;                          /* 确保步骤 1 写完 */
3. tlbi vae1, va >> 12;               /* local TLB 失效（旧 block entry） */
4. dsb ish;                            /* 确保 TLBI 完成 */
5. tlb_shootdown();                    /* IPI 广播到其它 CPU 跑 TLBI + ack 协议 */
6. alloc_4k_page() → l3_pa;            /* 分配 L3 table 页 */
7. zero_page(l3_pa);
8. for i in 0..511: pte[i] = inherit_from_block(old_block_desc, i); /* 软件位继承 */
9. dsb ishst;
10. pmd[l2] = l3_pa | encode_table_desc;  /* 写新 table 描述符 */
11. dsb ishst;
12. tlbi vae1, va >> 12;               /* local TLB 失效（新 table entries） */
13. dsb ish; isb;
unlock(pt_lock_for(root, va));
```

**非活跃 root**（仅 BSP pre-SMP 修改活跃 root 时）：步骤 5 可省略；步骤 3 仍需（避免 BSP 自己命中旧 entry）。

**pt_lock 设计**：per-(root, l0_idx, l1_idx) spinlock，避免不同 L2 段替换相互阻塞；映射是 root 粒度的。`kernel/lock/pt_lock.c` 新增，`arch_vmm_split_2m_to_4k` 在 step 0 拿、step 14 释放。x86 backend 暂不强制使用（x86 CR3 重载 + invlpg 无需此锁），但接口预留。

### 5.4 `aarch64_pt_split_block_2m(root, va)`

走 §5.3 协议；步骤 8 中 `inherit_from_block` 把 block 的 AP/SH/AttrIndx/AF/PXN/UXN + bit 55/56 移植到每个 4 KiB PTE。返回 OK。

### 5.5 merge 不纳入

F1。L3 table page 由 `free_4k_page` 在 unmap 时回收（caller 责任）；split 后该 L3 page 一直存在直至 L2 段全空后单独回收（M4 引入合并时实现）。

---

## 6. aarch64 TLB shootdown + IPI 接入（v2 修正）

### 6.1 当前状态

- `kernel/memory/tlb.c::tlb_shootdown()` 接口 arch-neutral，但**未编入 aarch64 build**（v1 误判）。
- `arch_flush_tlb_all()` aarch64 实现已存在：`kernel/include/arch/mmu.h:144-148`，`tlbi vmalle1 + dsb sy + isb`。
- `ipi_broadcast()` 当前仅有 x86 APIC 实现；aarch64 端 GIC SGI 收发路径已通（`docs/aarch64-ipi-fail-handoff-2026-09-26.md`），但缺 `arch_ipi_broadcast()` 通用入口。
- `IPI_VECTOR_TLB = 0x40` 是 x86 vector 编号，GICv2 SGI 编号 0..15，需做映射。

### 6.2 aarch64 IPI 发送 / handler 接入

**新增内容**：

1. **`kernel/include/intr/ipi.h`** 增加 arch-neutral 转换宏：

   ```c
   #define IPI_SGI_TLB    0   /* GICv2 SGIn=0；x86 端不使用，IPI_VECTOR_TLB=0x40 走 LAPIC */
   ```

2. **`kernel/arch/aarch64/intr/ipi.c`** 实现 `arch_ipi_broadcast(vector, exclude_self)`：

   - 若 `vector == IPI_VECTOR_TLB`：写入 `GICD_SGIR` 寄存器，SGI ID = `IPI_SGI_TLB`，target = all-but-self；
   - 其他 vector：panic（未实现）；
   - 复用现有 GIC 驱动（`kernel/arch/aarch64/intr/gic.c`）的 GICD 基址。

3. **`kernel/arch/aarch64/intr/tlb.c`**（或扩 `trap.c`）注册 `IPI_SGI_TLB` handler：

   ```c
   void aarch64_ipi_tlb_handler(void);
   /* 在 aarch64 启动路径注册到 IPI vector table 的 SGI 0 slot。 */
   ```

   处理：
   - `arch_local_irq_save()`；
   - `arch_flush_tlb_all()`（已有实现）；
   - `percpu_data[cpu].tlb_ack = 1`；
   - 清 `tlb_wanted`；
   - `arch_local_irq_restore()`。

4. **`kernel/memory/tlb.c` 编入 aarch64 build**：`kernel/Makefile` 与 aarch64 source group 加入 `tlb.c`。

5. **`kernel/arch/aarch64/boot/main.c`** 启动顺序增加：
   - `aarch64_ipi_init()`：注册 SGI handler；
   - 在 SMP online 之前完成（与现有 IPI init 同位置）。

### 6.3 TLB shootdown 调用链

caller 路径（`arch_vmm_*`）：
```
arch_vmm_* → 本地修改 PTE
            → aarch64_pt_invalidate_local(va) 或 aarch64_pt_invalidate_block_local(va)
            → tlb_shootdown()
                → 若 num_cpus > 1：
                    tlb_wanted/ack 协议 + arch_ipi_broadcast(IPI_VECTOR_TLB, 1)
                → 否则 local_only: arch_flush_tlb_all()
```

IPI handler 路径：
```
arch_ipi_broadcast → GIC SGI
                → aarch64_ipi_tlb_handler
                    → arch_flush_tlb_all()
                    → tlb_ack
```

### 6.4 死锁防护

同 v1 §6.2 #5：IPI handler 顺序与 x86 一致；`pt_lock` 不在 IPI handler 内持有；发起方在 IPI 等待期间不持 `pt_lock` 等待自身 ack（per-CPU 协议天然避免）。

### 6.5 性能策略

同 v1 §6.3：全表失效；per-VA shootdown 属 F2。

---

## 7. vmm_init 双后端

### 7.1 x86_64 端

原 `vmm_init()` 行为不变；x86 backend `arch_vmm_init()` 透传 + PMM walk + `tlb_shootdown()`。

### 7.2 aarch64 端

```c
int arch_vmm_init(void)
{
    uint64_t ttbr1 = aarch64_read_ttbr1();
    uint64_t pa = ttbr1 & AARCH64_TTBR_BASE_MASK;
    if (pa == 0 || pa >= AARCH64_PT_PA_LIMIT)
        return -EINVAL;
    kernel_map = (uint64_t *)(pa + ARCH_PAGE_OFFSET);
    return 0;
}
```

`kernel_map` 即 M1 直映根；所有 RAM 已可经 `kernel_map + offset` 访问。

### 7.3 `arch_vmm_free_user_map` 推迟到 M4

理由：aarch64 user PGD 由 M4 引入；M3 不实现该接口；x86 backend 保留原 `vmm_free_user_map` 实现（仅 x86 链接）。

---

## 8. 测试与验收（v2 修正）

### 8.1 M2 测试矩阵

| 阶段 | 测试 | 期望 | 文件 |
|----|----|----|----|
| RED | `test/hosttest/memory/test_slab_sizes.c`：16 size × 16 alloc/free pattern + free 后再 alloc 验复用 | 改动前 fail | 新增 |
| GREEN | 同上 | x86_64 host pass | 同 |
| BUILD | aarch64 build 编入 `slab.c`、移除 `slab_stub.c`、M1 arena 扩 Slab 预留 | build success | build profile 调整 |
| QEMU | `KERNEL_SELFTEST=1 qemu-system-aarch64 -smp 2 -m 1G` 启动到 main | selftest 全过；启动日志含 `slab_init: ok size=<bytes>`；M0 范围校验不 FATAL | QEMU launch |
| 回归 | x86_64 `test/hosttest/memory/` 全部 + `systest_repeat.py` 5 连 | 5/5 pass | 既有 |

### 8.2 M3 测试矩阵（v2 修正）

| 阶段 | 测试 | 期望 | 文件 |
|----|----|----|----|
| RED | `test/hosttest/memory/test_vmm_roundtrip.c`：**12 合法权限组合**（USER/KERNEL × RO/RW × EXEC/NOEXEC × NORMAL/DEVICE，扣除 DEVICE\|EXEC）+ **4 拒绝组合**（DEVICE\|EXEC × {RO,RW} × {USER,KERNEL} = 4 个 case，应返回 EINVAL）+ 软件位（VM_PROTNONE / VM_COW）保留 | 改动前 fail | 新增 |
| RED | `test/hosttest/memory/test_vmm_split.c`：构造 2 MiB block，split 后逐 PTE 验 PA + 权限 + 软件位；break-before-make 顺序断言（lock acquire 早于 step 1，release 晚于 step 13） | 改动前 fail | 新增 |
| RED | `test/hosttest/memory/test_vmm_init.c`：调 `arch_vmm_init`，x86 验证 kernel_map 非空 + PMM walk 完成；aarch64 验证 kernel_map = M1 TTBR1 root | x86 通过，aarch64 fail | 新增 |
| RED | `test/hosttest/memory/test_vmm_prot_none.c`：构造 Valid=0 + PROTNONE PTE，query 返回 `EPROT_NONE` 并给 PA；unmap 释放 PA；free 时正确处理 | 改动前 fail | 新增 |
| GREEN | x86_64 + aarch64 host 全过 | 12+4+软件位+split+init+PROT_NONE 全过 | 同 |
| QEMU 单核 | `KERNEL_SELFTEST=1 aarch64 -smp 1` selftest：`arch_vmm_map_4k_new` / `update_4k` / `unmap_4k` / `map_2m` / `unmap_2m` / `split_2m_to_4k` / `pt_walk` 单元路径 | 全过 | selftest 命令 |
| QEMU 多核 | 跨 CPU 共享页测试：CPU A `arch_vmm_map_4k_new` 新页 → CPU B `*(volatile uint32_t *)va` 读到正确值；IPI 等待不死锁（双核超时 5s） | `aarch64 -smp 2/4` 全过 | selftest |
| QEMU PT_NONE | selftest：构造 PROT_NONE 页 → query 拿到 PA → unmap 释放 → query ENOENT | 全过 | selftest |
| 回归 | x86_64 `systest_repeat.py` 5 连 | 5/5 pass | 既有 |
| 排除验证 | aarch64 build 不引用 `vma.c` / `uaccess.c` / `task.c` fork PTE 段 | build success + 符号检查 | make 验证 |

### 8.3 验收门槛（v2 修正）

- **M2**：hosttest 全部 GREEN + aarch64 `KERNEL_SELFTEST=1` QEMU 启动成功 + 启动日志含 `slab_init: ok size=<bytes>` + M0 范围校验不 FATAL + x86_64 systest_repeat 5/5。
- **M3**：hosttest 全部 GREEN（12+4+软件位+split+init+PROT_NONE）+ aarch64 QEMU 单核+多核 selftest 通过 + x86_64 systest_repeat 5/5。
- **不要求**：aarch64 shell、aarch64 systest_repeat（M4+ 再做）。
- 任意 hosttest RED 不得 merge；任意 QEMU 失败必须 fix 后再跑。
- M1 验收（16 组 RAM/CPU/镜像矩阵、稀疏/容量耗尽/AP 无效 root 注入）维持不回归。

---

## 9. 风险与遗留（v2 修正）

### 9.1 风险

**R1**（v2 修正）：M1 arena 未扩 Slab 预留会与 M1 表页池冲突或 PMM 把 Slab 预留页分配出去。**对策**：§3.2 已纳入；host test 加 arena 大小边界检查。

**R2**：aarch64 全表 TLBI 性能。**对策**：M3 接受；F2 推迟。

**R3**：split 后 L3 table page 不回收。**对策**：M3 不实现 merge；F1。

**R4**：aarch64 VM 软件位 bit 55/56 真硬件兼容性。**对策**：M3 仅 QEMU 验证。

**R5**：aarch64 `arch_vmm_init` 把 M1 root 作为 kernel_map 后行为分裂。**对策**：`vmm_init` 注释明示。

**R6**（v2 修正）：`aarch64_pt_query_4k` 遇 Valid=0 返回 ENOENT，PROT_NONE 无法 query PA。**对策**：§4.6 已纳入；新增 `AARCH64_PT_EPROT_NONE` 返回值。

**R7**（v2 新增）：break-before-make 协议中，步骤 5 `tlb_shootdown()` 等待其它 CPU ack 时，`pt_lock` 仍持有。若其它 CPU 因同一锁死锁（IPI handler 不持此锁），不会直接 deadlock；但若 caller 在持锁期间等待其它 CPU ack 而其它 CPU 又在等同一锁（不在 IPI handler 而在普通执行路径），会真 deadlock。**对策**：`pt_lock` 设计为 per-(root, l0_idx, l1_idx)，同一锁的临界区不跨 tlb_shootdown ack 等待——`tlb_shootdown` 完成后才 unlock（当前协议已是如此）；host test 加 `pt_lock` 嵌套场景验证。

**R8**（v2 新增）：aarch64 IPI handler 与 GIC 中断嵌套。SGI 处理期间可能嵌套更高优先级中断（如 GIC PPI）。**对策**：handler 内 `arch_local_irq_save/restore` 已屏蔽本地 IRQ；嵌套优先级由 GIC 配置保证；M3 验证 GIC priority 不被 SGI handler 破坏。

**R9**（v2 新增）：caller 范围限制执行不严。`vma.c` / `uaccess.c` 在 aarch64 build 中残留（仅 ifdef 排除）可能在编译期未发现，链接期才发现。**对策**：`make` 引入 `aarch64_vma_excluded` stamp + `nm` 检查 `vma_*` 符号不在 aarch64 kernel image 中。

### 9.2 Follow-up（不属本 spec）

- **F1**：merge_4k_to_2m。
- **F2**：per-VA TLB shootdown。
- **F3**：旧 `PAGE_*` alias 删干净（v2 直接不保留 alias）。
- **F4**：slab `kmalloc_creating` 递归检测。
- **F5**：caller 库（`vma.c` / `uaccess.c` / `task.c` fork 段）在 aarch64 的重写 —— M4 范围。
- **F6**：M4 用户地址空间（user PGD / EL0 切换 / uaccess 故障恢复 / `arch_user_range_accessible` 接通真实跨页权限检查）。
- **F7**：aarch64 user PGD / `arch_vmm_free_user_map` —— M4 范围。
- **F8**：x86_64 backend 是否迁到 `alloc_4k_page/free_4k_page` 表页 —— 与 slab/PMM 解耦后单独评估。

---

## 10. 实施拆分（v2 修正）

M2 + M3 同一份 plan，9 个子任务（含 1 个新前置）。

1. **PRE.0**：v2 spec 评审通过 + `aarch64_pt_map_4k/unmap_4k/query_4k` 扩展 `software_bits` 参数 + 新增 `AARCH64_PT_EPROT_NONE` 返回值（host test 加 RED）。**这是 M3 的真正起跑线**，未做则 M3.1 之后无法进展。
2. **M2.1**：`slab.c` 锁路径替换；新增 `slab_estimate_size()`；host `test_slab_sizes.c` RED→GREEN。
3. **M2.2**：M1 arena 扩展 Slab 预留；`aarch64_early_arena_init` 调用 `slab_estimate_size()` + boot reservation 注册 + M0 范围 FATAL 检查。aarch64 build 编入 `slab.c` + 删除 `slab_stub.c`。QEMU `KERNEL_SELFTEST=1` 启动验证。
4. **M3.1**：vmm.h flag 重命名（删除旧 `PAGE_*`） + per-arch vmm_backend 接口骨架；x86_64 backend 搬实现 + 旧 wrapper 路由；caller 迁移（vma/uaccess/fork 段在 aarch64 build 排除）。host `test_vmm_roundtrip.c` RED→GREEN。
5. **M3.2**：aarch64 IPI 接入 + `tlb.c` 编译接入 + aarch64 TLB handler 注册 + `arch_ipi_broadcast` 实现。QEMU 双核 IPI 通信自测。
6. **M3.3**：aarch64 vmm_backend 4 KiB 路径（`map_4k_new` / `update_4k` / `unmap_4k` / `pt_walk`） + 软件位 round-trip。host `test_vmm_roundtrip.c` GREEN（aarch64 部分）+ `test_vmm_prot_none.c` RED→GREEN。
7. **M3.4**：aarch64 `aarch64_pt_map_2m_block` / `unmap_2m_block` + `aarch64_pt_invalidate_block_local` + `arch_vmm_map_2m` / `unmap_2m`。host 边界测试。
8. **M3.5**：aarch64 `aarch64_pt_split_block_2m` + `arch_vmm_split_2m_to_4k` + `pt_lock` + break-before-make 协议。host `test_vmm_split.c` RED→GREEN。
9. **M3.6**：aarch64 `arch_vmm_init` 注册 M1 root + `test_vmm_init.c` RED→GREEN。
10. **M3.7**：回归 — x86_64 systest_repeat 5 连 + aarch64 `KERNEL_SELFTEST=1` QEMU 单核+多核 selftest 全过 + caller 排除验证。

每个子任务独立 RED→GREEN→REFACTOR；失败不进入下一个。
