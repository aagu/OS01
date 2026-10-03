---
title: OS01 aarch64 M2 Slab + M3 运行期 VMM 设计 (v3)
created: 2026-10-03
type: spec
status: revised-for-review
tags: [osdev, aarch64, memory, slab, vmm, tlb, ipi]
supersedes: 8626553 (v2), 93f8ae7 (v1)
---

# aarch64 M2 Slab + M3 运行期 VMM (v3)

> v3 修订 v2 评审的 16 项问题（9 阻塞 + 4 重要 + 3 文档/验收）。v2 修掉了 v1 的基线错误，但在三大契约——**slab 物理布局与记账**、**生产 SGI/shootdown**、**页表替换与页所有权**——上仍是不可实施的。v3 重做这三个契约，并修正接口与测试矩阵。

## 0. v3 关键修订摘要

| # | v2 错误 | v3 修正 | 落点 |
|---|----|----|----|
| 1 | slab 空间预估漏掉 16 个 color bitmap（合计 ≈16,416 B）；把 BSS 静态变量计入 arena；错称 8 页来自 8 次 `alloc_pages` | 布局从 `slab_init` 的**实际地址推进规则**推导（16 组 `sizeof(struct Slab)+10×long+color_length+10×long`，color_length 随 cache size 变化）；BSS 不计入；8 页是**按地址直接占用**（`slab_page_start = align2M(end_of_struct)` 后连续 8 个 2 MiB） | §3.2 |
| 2 | §2 要求 slab 页"不与 arena 重叠"、§3.2 又要求"纳入 arena"；boot reservation 整体预留 + slab_init 无条件记账 = **双重计数** | 单一记账路径：boot reservation 标 **PMM 元数据段 + M1 表页池**，**排除 slab 段**（slab 元数据帧 + 8 个 slab 页由 `slab_init` 自己记账，保持 x86 字节级同行为）；arena 尺寸含 slab 段以保证区间互不重叠 | §3.2 |
| 3 | M0 越界检查放在 `arch_boot_direct_map_init`（在 `pmm_init`/真实 slab_init **之后**），越界写已发生 | 全部检查（连续 RAM、M0 覆盖上界、区间不重叠、容量）移到 **arena preflight**（`pmm_init` 之前） | §3.2 |
| 4 | TLB IPI 选 SGI 0，与 `ipi_test.c` 的 `IPI_SGI_ID=0`/`IPI_REPLY_SGI=1`/clobber SGI 2 冲突 | TLB 用 **SGI 3**；加入 `gic.c` 每核 banked 白名单；验收要求 IPI 自测与 TLB IPI 并存工作 | §6.2 |
| 5 | 正式构建 AP 在 `secondary_idle` 中**保持 IRQ 屏蔽**（`#if OS01_SELFTEST` 才 enable），shootdown 的 ack 永远等不到 | M3 纳入**正式构建 AP IRQ 使能**（boot command 处理完后开 DAIF.I 进 SGI 待命循环）；验收必须含**非 selftest 镜像**的双核 shootdown | §6.2/§6.4 |
| 6 | break-before-make 先清 block 后分配 L3，`alloc_4k_page` 失败留下 2 MiB 永久空洞 | **先分配并填好新 L3，再持锁 break/make**；每个失败点定义回滚 | §5.3 |
| 7 | `update_4k` 有效 PTE 直接换 PA/权限无安全协议；只有 block↔table 有协议 | 4 KiB 更新协议：同 PA 改权限 = 原子 8 B store + dsb + TLBI + shootdown；换 PA = break-before-make（清→失效→写新）；纯软件位变更拆成独立 `arch_vmm_set_software_4k` | §4.4.3 |
| 8 | 单个 `tlb_wanted/tlb_ack` 槽无法承受多 CPU 并发请求；超时后静默继续 | **发起方串行化**（shootdown 自旋锁）+ **代数式 ack**（per-CPU ack generation counter）；超时 = **FATAL panic**，不再当作已失效 | §6.4 |
| 9 | unmap PROT_NONE 在后端内 `free_4k_page`，普通页/2 MiB 页却返回 PA 由 caller 释放——所有权规则自相矛盾 | **统一：unmap 只摘除映射**，返回 `phys_out` + `sw_out`，永不 free；释放/COW 引用计数由拥有该页的上层做 | §4.4.4 |
| 10 | §4.2 与 §4.3 两套不兼容签名；`VM_PROTNONE` 同时是 vm_flags 位和独立 `vm_software_t` | **一套签名**：软件位就是 `vm_flags` 的位（`VM_PROTNONE`/`VM_COW`）；`VM_PRESENT=0 + VM_PROTNONE=1` 定义为合法"无效但持有 PA"状态；unmap 用 out 参数返回旧软件位 | §4.2/§4.3 |
| 11 | "删除全部 PAGE_*" 与 x86 fork/fault/uaccess/VMA 直接操作硬件描述符的现实冲突；`arch_vmm_pt_walk` 返回裸 PTE 指针却放在公共头 | x86 硬件位**留在 x86 私有头**（`kernel/include/arch/x86_64/pte.h`）；公共 `vmm.h` 只放语义 API + `VM_*`；保留裸 PTE 访问的 x86 文件逐个列出（vma/uaccess/task.c fork 段/do_page_fault/boot_direct_map）；M3 不强制迁移 x86 内部代码 | §4.5 |
| 12 | 试图按行号范围排除 `task.c`——构建系统只能整文件 | `task.c` **整个文件**保持不编入 aarch64（当前 `kernel/Makefile:39` 白名单本来就不含 `sched/`）；M4 若需其中功能先拆文件 | §4.5 |
| 13 | `arch_vmm_init` 无生产调用点 | 明确：aarch64 `boot/main.c` 中 `arch_boot_direct_map_init()` 成功返回后立即调用；失败 FATAL 早停；测试断言 `kernel_map == 当前 TTBR1 root` | §7.2 |
| 14 | 多核测试让 CPU B"直接读 VA"——AP 无执行该工作的机制；首次读也无法证明旧 TLB 被失效 | 跨核测试用 **boot command 工作项机制**（复用 `AARCH64_BOOT_GO_TEST`/`smp_bench_iter` 同款通道）：B 先在旧映射下读并记录 → A 替换/解除映射并等 ack → B 再读验证 → B 置结果标志 → A 检查；覆盖 -smp 2/4 | §8.2 |
| 15 | 测试路径写不存在的 `test/hosttest/memory/`；`test_vmm_prot_none` 验证已推迟到 M4 的 `free_user_map` | 测试落在 **`hosttests/cases/`**（实际 harness）；M3 测试只验证本阶段承诺的 API | §8 |
| 16 | R7 风险的"对策"与风险描述相同（就是现状流程） | 明确：`pt_lock` 以 `spin_lock_irqsave` 获取；IPI handler **不取任何 pt_lock**；持锁等待 ack 的 CPU 不会阻碍目标 CPU 执行 handler（IRQ 关只屏蔽本核中断，不影响他核）；双 CPU 竞争同一页表区域的测试验证 | §5.3/§9 R7 |

---

## 1. 意图、基线与完成标准

依据 roadmap P2，M2 让 aarch64 kernel 从 `kmalloc()=NULL` 走到能在任意缓存大小上分配/释放；M3 让运行期既可映射/解除映射 4 KiB 也可创建/拆分 2 MiB block，并保留 x86_64 全部现行行为。M2+M3 是 M4 用户地址空间的前置；M4 不在本 spec。

**基线**：master @ `dabc9f0`。前置依赖 M0 ✅ / M1 ✅ / Generic Timer ✅ / GICv2 Phase 1 ✅ / IPI fix ✅ / PMM arch-neutral ✅。

### 1.1 aarch64 启动顺序实测

`kernel/arch/aarch64/boot/main.c`：

1. `aarch64_ram_init()` + M1 preflight / `aarch64_early_arena_init()`（≈:388-405，**在 `pmm_init` 之前**）。
2. `pmm_init(handoff)`（:411）→ 内部调 `slab_init()`（`pmm.c:317`）。
3. `arch_boot_direct_map_init()`（:422）：M1 直映安装，**在 Slab 之后**。

即真实 Slab 运行在 **M0 直映（TTBR0，0..2 GiB）** 下。Slab 元数据与 8 个预分配 2 MiB 页的物理地址必须在 [0, 2 GiB) 且被正确记账（§3.2）。

### 1.2 M2 完成标准

- aarch64 build 编入 `kernel/memory/slab.c`，移除 `kernel/arch/aarch64/runtime/slab_stub.c`。
- `slab.c` 锁路径换 `arch_local_irq_save/restore`；`slab.c` 内不再出现 `pushfq/cli/sti/popfq` 字面。
- **布局与记账契约**（§3.2）：arena preflight 在 `pmm_init` 之前完成 slab 段的容量/M0 覆盖/互不重叠检查；slab 帧只被 `slab_init` 记账一次；启动后断言实际 `end_of_struct ≤ 预估上界`。
- aarch64 `KERNEL_SELFTEST=1` QEMU 启动到 main 不 panic；selftest 跨 16 缓存大小 kmalloc/kfree pass。
- x86_64 hosttests + `systest_repeat.py` 5 连 pass（slab 行为字节级不变）。

### 1.3 M3 完成标准

- **IPI/shootdown 契约**（§6）：aarch64 SGI 3 = TLB；正式构建 AP 使能 IRQ 并服务 SGI；`tlb_shootdown` 发起方串行化 + 代数 ack + 超时 FATAL；`kernel/memory/tlb.c` 编入 aarch64。
- **vmm 语义层**（§4）：公共 `vmm.h` 只含 `VM_*` 语义位与语义 API；x86 硬件位迁至 x86 私有头；`vma.c`/`uaccess.c`/`task.c` 保持不编入 aarch64。
- **替换协议**（§4.4.3/§5.3）：4 KiB 更新、block↔table 替换均有 break-before-make 或等价安全序列；split 失败不留空洞。
- **所有权契约**（§4.4.4）：unmap 永不 free。
- aarch64 `aarch64_pt_*` 扩展：block 编解码、split、软件位（`VM_PROTNONE`/`VM_COW` 进出原语）、`AARCH64_PT_EPROT_NONE`。
- aarch64 `arch_vmm_init` 有生产调用点（§7.2）。
- host 边界测试（12 合法 + 4 拒绝组合 + split + 软件位 + PROT_NONE）+ QEMU 单核/多核（含**非 selftest 镜像**）selftest。
- x86_64 `systest_repeat.py` 5 连 pass。

### 1.4 不属于本 spec

M4 用户地址空间（user PGD / EL0 切换 / uaccess 故障恢复 / `arch_vmm_free_user_map`）；`merge_4k_to_2m`（F1）；per-VA shootdown（F2）；`vma.c`/`uaccess.c`/`task.c` 的 aarch64 重写（M4）；Slab 算法优化；ASLR。

---

## 2. 已核实的约束

1. `slab.c:36-55` 锁路径为 inline `pushfq; cli` / `sti`；`arch_local_irq_save/restore` 已存在（`kernel/include/arch/irq.h`）。
2. `slab_init`（`slab.c:336-415`）实际行为：
   - 在 `PMMngr.end_of_struct`（PMM 自己的元数据末尾）之后，为 16 个 cache 依次写入 `struct Slab` + `10×long` 填充 + `color_length` 字节的 color bitmap（32 B 缓存的 bitmap 为 `PAGE_2M/32/8 = 8 KiB`；16 组合计含对齐填充约 16.4 KiB），**BSS 中的 `kmalloc_cache_size[16]` 等静态变量不占 arena**；
   - 元数据写完后，把覆盖到的所有 2 MiB 帧用 `bits_map |=`、`using_count++`、`free_count--`、`page_init(PG_PTable_Mapped|PG_Kernel_Init|PG_Kernel)` **无条件记账**；
   - `slab_page_start = align_up(end_of_struct, 2 MiB)` 后**按地址直接占用**连续 8 个 2 MiB 帧（cache 0-7 预分配；cache 8-15 首次 kmalloc 时经 `alloc_pages` 取页），同样无条件记账。
3. PMM boot reservation（`pmm.c:671` `pmm_reserve_boot_ranges`）：对范围内帧做幂等预留（已置位则跳过）。若把 slab 段也交给它预留，`slab_init` 随后的无条件 `using_count++/free_count--` 会**双重计数**。
4. aarch64 启动顺序：`main.c:411 pmm_init` → `pmm.c:317 slab_init` → `main.c:422 arch_boot_direct_map_init`。
5. M0 直映范围 [0, 2 GiB)（TTBR0，head.S）；M1 arena 逻辑（`early_arena.c`）目前只计入 PMM 元数据 + M1 表页。
6. GIC 白名单（`gic.c:25-30`）：SGI 0（`ipi_test.c::IPI_SGI_ID`）+ SGI 1（`IPI_REPLY_SGI`）+ SGI 2（clobber 探针）+ CNTP PPI。**SGI 3 空闲**。
7. `secondary_idle`（`smp.c:244-263`）：AP 处理完 boot command 后，**仅 `OS01_SELFTEST` 构建开 DAIF.I**，正式构建保持 IRQ 屏蔽并 `halt` 循环。
8. `tlb_shootdown`（`tlb.c:22-65`）：单 `tlb_wanted`/`tlb_ack` 槽；ack 超时后 `debug_mm` 打印并**继续执行**（当作已失效）；未编入 aarch64 build。
9. `IPI_VECTOR_TLB = 0x40` 是 x86 vector；GICv2 SGI ∈ [0,15]，需映射。
10. `arch_flush_tlb_all` aarch64 实现已存在（`kernel/include/arch/mmu.h:144`，`tlbi vmalle1 + dsb sy + isb`）；`arch_flush_tlb_page` 同文件已有。
11. `aarch64_pt_map_4k` 遇有效 PTE 返回 `EEXIST`（不覆盖）；x86 `vmm_map_4k_page` 直接覆盖。
12. `aarch64_pt_query_4k` 遇 `Valid=0` 返回 `ENOENT`；PROT_NONE（V=0 + 软件位）取不到 PA。
13. `encode_perm`（`page_table.c:238`）拒绝 `AARCH64_PT_PERM_ALL_BITS` 之外的位——软件位现在进不了原语。
14. 直接操作 x86 硬件 PTE 的文件：`memory/vma.c`、`memory/uaccess.c`、`sched/task.c`（fork PTE 拷贝）、`arch/x86_64/memory/boot_direct_map.c`（经 checked helper）、x86 `do_page_fault` 路径。当前 aarch64 source gate（`kernel/Makefile:39-47`）为白名单式，**均未编入**。
15. `aarch64/page_table.h:10` 限制活跃 root 仅 BSP pre-SMP 修改。

---

## 3. M2：slab 接入与物理布局/记账契约

### 3.1 锁路径替换

同 v2 §3.1（`arch_irq_state_t` + `arch_local_irq_save/restore`），不重复。

### 3.2 布局与记账契约（v3 重写）

**单一事实来源**：新增共用布局计算函数，从 `slab_init` 的地址推进规则**精确推导**（不是估算常数）：

```c
/* kernel/include/memory/slab.h */
struct slab_layout {
    uint64_t meta_bytes;       /* 16 × (sizeof(struct Slab) + 10*long
                                  + color_length(size) + 10*long, 对齐到 long) */
    uint64_t reserved_2m_pages; /* 8（cache 0-7 预分配） */
};
struct slab_layout slab_layout_compute(void);
/* 纯函数：只读 kmalloc_cache_size[] 的 size 字段与类型布局，
 * 不触碰任何内存。arena preflight 与 slab_init 共用。 */
```

**物理区间图**（arena 内，物理地址升序，互不重叠）：

```
[PMM 元数据段][slab 元数据][对齐填充][8 × 2 MiB slab 预分配页][M1 表页池]
                                                ↑
                        slab_page_start = align_up(PMM meta + slab meta, 2 MiB)
```

**记账规则（每帧恰好一条路径）**：

| 区间 | 记账者 | 时机 |
|----|----|----|
| PMM 元数据段 | `pmm_init`（现状） | `pmm_init` 内 |
| slab 元数据覆盖的 2 MiB 帧 | `slab_init`（现状无条件记账，保持 x86 字节级同行为） | `pmm_init` 内、紧随 PMM 元数据 |
| 8 × 2 MiB slab 预分配页 | `slab_init`（现状） | 同上 |
| M1 表页池 | boot reservation（`pmm_reserve_boot_ranges`，现状） | `pmm_init` 内 |

**关键修正**：boot reservation 的 range 列表**必须排除 slab 段**（slab 元数据帧 + 8 个预分配页）；arena 总尺寸**必须包含 slab 段**（保证 `slab_page_end ≤ arena_end`，且 slab 页不会被 M1 表页池或后续分配抢占——`slab_init` 在任何 `alloc_pages` 调用者之前运行，时序上无人能偷走，尺寸保证是防重叠的结构性约束）。

**Preflight 检查（全部在 `pmm_init` 之前，`aarch64_early_arena_init` 内）**：

1. arena 扩展后总长 = PMM 元数据 + slab 元数据 + 8×2 MiB + M1 表页池，仍落在连续 RAM 区间内（沿用现有连续性检查）；
2. **整个 arena（含 slab 段）物理上界 < 2 GiB**（M0 直映覆盖）；越界 = FATAL，打印需求/可用，**在任何内存写入前停机**；
3. slab_page_start + 8×2 MiB ≤ arena_end；
4. 容量断言：`slab_layout_compute()` 的 `meta_bytes` 与预flight 常量一致（编译期 `_Static_assert` 无法做——size 是运行期数组，改运行期 assert）。

**启动后验证**：`slab_init` 末尾断言 `实际 end_of_struct ≤ PMM_meta_end + layout.meta_bytes`；boot reservation 应用后断言 slab 段帧未被双重置位（抽查 slab 段首帧 `bits_map` 位在 reservation 后、slab_init 前为 0）。

### 3.3 aarch64 build 接入

`kernel/Makefile:39-47` aarch64 白名单加 `memory/slab.c`；删除 `kernel/arch/aarch64/runtime/slab_stub.c`。

### 3.4 不动项

同 v2 §3.4。

---

## 4. vmm 语义层与接口（v3 重写）

### 4.1 两层分离

- **公共语义层**（`kernel/include/memory/vmm.h`）：`VM_*` 语义位 + 语义 API。任何 arch 的通用代码只 include 这一层。
- **x86 硬件层**（`kernel/include/arch/x86_64/pte.h`，新建）：现有 `PAGE_*` 位定义 + `vmm_pt_walk`（返回裸 PTE 指针）+ `vmm_free_user_map` 迁入。**仅 x86-only 文件 include**。

### 4.2 语义位（唯一一套表示）

```c
/* vmm.h —— 位位置由 arch/<arch>/vmm_backend.h 提供，caller 不感知 */
#define VM_PRESENT   ...   /* 有效映射 */
#define VM_WRITE     ...   /* 可写 */
#define VM_USER      ...   /* EL0 可达 */
#define VM_NO_EXEC   ...   /* 不可执行（缺省语义：不含此位 = 可执行） */
#define VM_HUGE      ...   /* block 描述符 */
#define VM_NOCACHE   ...   /* 设备/UC */
#define VM_PROTNONE  ...   /* 软件位：无效但持有 PA（mprotect(PROT_NONE) 暂存） */
#define VM_COW       ...   /* 软件位：COW 共享，写触发 fault */

#define VM_KERNEL_RW (VM_PRESENT | VM_WRITE)
#define VM_KERNEL_RO (VM_PRESENT)
#define VM_USER_RW   (VM_PRESENT | VM_WRITE | VM_USER)
#define VM_USER_RO   (VM_PRESENT | VM_USER)
#define VM_DEVICE    (VM_PRESENT | VM_WRITE | VM_NOCACHE)
```

**`VM_PRESENT=0 且 VM_PROTNONE=1`** 是合法状态："映射无效但 PA 归其所有"；backend 编码为 V=0 + 软件位；`query` 对它返回 `EPROT_NONE` + PA。

### 4.3 语义 API（唯一一套签名）

```c
int   arch_vmm_init(void);
/* 新建：遇任何已占用（含 PROT_NONE）返回 -EEXIST，不覆盖 */
int   arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
/* 替换：按 §4.4.3 协议安全替换；返回旧 PA 与旧软件位 */
int   arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags,
                         uint64_t *old_phys_out, uint32_t *old_vm_out);
/* 摘除：只清映射，永不 free；返回 PA 与软件位（含 EPROT_NONE 状态） */
int   arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt,
                        uint64_t *phys_out, uint32_t *old_vm_out);
/* 纯软件位变更（PROT_NONE 暂存/恢复、COW 标记/清除），不动 PA 与硬件权限 */
int   arch_vmm_set_software_4k(uint64_t *pgdir, uint64_t virt, uint32_t set, uint32_t clear);
/* 查询：覆盖有效 / PROT_NONE / ENOENT 三态 */
int   arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt,
                        uint64_t *phys_out, uint32_t *vm_out);

int   arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);
/* arch_vmm_pt_walk / arch_vmm_free_user_map 不进公共层 —— 前者返回裸 PTE 指针，
 * 迁入 x86 私有头；后者推迟 M4。 */
```

x86 旧符号（`vmm_map_page` 等）留在 x86 私有头或 wrapper，仅 x86 build 可见。

### 4.4 后端行为契约

#### 4.4.1 aarch64：表页与原语

- 表页一律 `alloc_4k_page()` / `free_4k_page()`（PMM 路径，不依赖 Slab）。
- `aarch64_pt_map_4k / query_4k / unmap_4k` 扩展：签名增加软件位（进：`vm_flags` 直译为描述符 bit 55/56；出：`software_bits_out`）；新增返回值 `AARCH64_PT_EPROT_NONE`（V=0 + bit 55 + PA 有效，`query/unmap` 返回 PA）；`encode_perm` 之外的软件位由 backend 组合，不改 `AARCH64_PT_PERM_ALL_BITS` 语义。
- 新增 `aarch64_pt_map_2m_block / unmap_2m_block / split_block_2m`（§5）。

#### 4.4.2 x86_64：表页与行为保持

- 表页保持 `calloc`/`kfree`；`vmm_init` PMM walk、`kernel_map = Phy_To_Virt(0x101000)`、覆盖语义 wrapper 全部不变，仅迁到 x86 backend/私有头。

#### 4.4.3 替换协议（`arch_vmm_update_4k`，v3 新写）

设旧描述符 D_old（含 PA_old、权限、软件位），新请求 D_new：

| 变化内容 | 协议 |
|----|----|
| 仅软件位 | 转走 `arch_vmm_set_software_4k`。注意置 `VM_PROTNONE` 语义上要求条目对硬件**不命中**（V=0），因此该 API 允许伴随翻转折符的 V 位——执行序列与下述"有效 ↔ PROT_NONE"行相同（break-before-make）；清除 `VM_PROTNONE` 则恢复原硬件权限。`VM_COW` 置/清只动 bit56 与 W 位，属同 PA 权限变，走原子 store + TLBI + shootdown |
| 同 PA，权限/属性变 | 原子 8 B store 写 D_new → `dsb ishst` → local `tlbi vae1` → `dsb ish` → shootdown。有效→有效替换，无窗口 |
| PA 变（重映射） | **break-before-make**：清 PTE → `dsb ishst` → local TLBI → `dsb ish` → shootdown → 写 D_new → `dsb ishst` → local TLBI → `dsb ish; isb`。持 `pt_lock` 期间执行 |
| 有效 ↔ PROT_NONE | 同"PA 变"路径（描述符有效位翻转，中间必须无硬件可命中的错误翻译） |

`update_4k` 返回 `old_phys_out` + `old_vm_out`；**释放旧 PA 由 caller 决定**。

#### 4.4.4 所有权契约（v3 统一）

**后端永不释放数据页。** `unmap_4k` / `unmap_2m` / `update_4k` 只摘除/替换映射并返回旧 PA 与旧软件位。`free_4k_page` / `free_pages` / COW 引用计数（`page_cow_put`）由拥有该页的上层（M4 前仅 x86 的 vma/uaccess/fork 路径；aarch64 侧 M3 测试代码）执行。PROT_NONE 页同理：unmap 返回其 PA，caller 释放。

#### 4.4.5 aarch64 `unmap_2m` 契约（v3 补全）

- `pmd[l2]` 必须是 block（V=1, bit1=0）；遇 L3 table 描述符返回 `-EEXIST` 改为 `-EINVAL`（类型错误，非占用冲突）；遇无效返回 `-ENOENT`。
- `*phys_out = desc & PA_MASK`；清条目；`dsb ishst` → block local TLBI → `dsb ish` → shootdown；不释放数据页；L2 表页保留。

### 4.5 caller 范围（v3 修正）

**M3 保持不编入 aarch64**（当前 `kernel/Makefile:39` 白名单已排除，零改动，仅写死契约）：

- `kernel/memory/vma.c`（整文件）
- `kernel/memory/uaccess.c`（整文件）
- `kernel/sched/task.c`（整文件——构建系统只能整文件粒度；M4 需要非 fork 功能时先拆文件再选编）
- 其余直接操作 x86 PTE 的文件（x86 `do_page_fault` 等）本就是 x86-only

**保留裸 PTE 访问的 x86 文件**（include `arch/x86_64/pte.h`）：`vma.c`、`uaccess.c`、`task.c`、x86 `do_page_fault` 路径、`arch/x86_64/memory/boot_direct_map.c`、`kernel/memory/vmm.c`（x86 部分）。M3 **不迁移**这些文件的内部实现，只迁头文件依赖。公共 `vmm.h` 中 `PAGE_*` 全部删除。

---

## 5. aarch64 block 与 split（v3 重写）

### 5.1 block 描述符编码

同 v2 §5.1（bit1=0 区分 block；AP/SH/AttrIndx/PXN/UXN/AF 与 leaf 同编码；软件位 bit 55/56）。

### 5.2 map/unmap 2 MiB block

同 v2 §5.2，unmap 契约以 §4.4.5 为准。

### 5.3 split 协议（v3 重写：先建后拆，失败无空洞）

```
 0. l3_pa = alloc_4k_page();            /* 先分配 */
    if (!l3_pa) return -ENOMEM;          /* 映射完好，无副作用 */
 1. zero_page(l3_pa);
 2. for i in 0..511:                     /* 先填好新表 */
        pte[i] = inherit(block_desc, i); /* PA+i*4K、AP/SH/AttrIndx/AF/PXN/UXN、
                                            软件 bit55/56 全继承 */
 3. dsb ishst;
 4. lock(pt_lock, irqsave);              /* §5.4 */
 5. pmd[l2] = 0;                          /* BREAK：旧 block 失效 */
 6. dsb ishst;
 7. tlbi vae1, va>>12; dsb ish;
 8. shootdown();                          /* 跨核失效旧 block 翻译 */
 9. pmd[l2] = l3_pa | table_desc;         /* MAKE：发布新表 */
10. dsb ishst;
11. tlbi vae1, va>>12; dsb ish; isb;
12. unlock(pt_lock, irqrestore);
```

**失败点与回滚**：步骤 0-3 失败（分配/清零）→ 释放 l3_pa、返回错误，原映射未动。步骤 4 之后无分配，不再有失败路径。非活跃 root（构造中的树）：步骤 7/8 可省略（无人可能持有旧翻译）。

### 5.4 pt_lock 与死锁论证（v3 重写，修 #16）

- `pt_lock`：per-(root, l0_idx, l1_idx) 自旋锁，`spin_lock_irqsave` 获取。
- **IPI handler 不取任何 pt_lock**：TLB handler 只做 `tlbi vmalle1` + ack 写，无锁。
- 死锁论证：CPU A 持 pt_lock 等 ack 时，目标 CPU B 只需执行 IPI handler 即可 ack——handler 无锁、无依赖，B 即使正自旋等同一把 pt_lock（在普通路径），其 IRQ 仍开（B 未持锁那侧），SGI 可抢占自旋并 ack。A 自己关 IRQ 不影响 B。
- 验证测试：双 CPU 并发对**同一** 2 MiB 段做 split/update，双方都须完成（§8.2）。

---

## 6. aarch64 生产 SGI / shootdown 契约（v3 重写）

### 6.1 SGI 分配

| SGI | 用途 | 状态 |
|----|----|----|
| 0 | `ipi_test` 主载荷 | 已占用 |
| 1 | `ipi_test` 回发确认 | 已占用 |
| 2 | `ipi_test` clobber 探针 | 已占用 |
| **3** | **TLB shootdown** | **M3 新增** |

`gic.c:25-30` 白名单加 SGI 3（每核 banked enable）。验收含 IPI 自测与 TLB IPI 并存。

### 6.2 正式构建 AP IRQ 使能（v3 新增，修 #5）

`secondary_idle`（`smp.c`）改为：boot command 处理完后，**生产与 selftest 构建都**执行 `arch_local_irq_enable() + isb`，随后 halt 循环（wfi/halt 均可——SGI 会唤醒）。理由：M3 起 AP 承担 TLB shootdown 应答义务，永久关 IRQ 的 AP 会让任何多核映射修改挂死。此改动使 `#if OS01_SELFTEST` 门移除；ipi_test 行为不变（其假设"AP 开 DAIF.I 等 SGI"由特殊变特殊变普遍成立）。

### 6.3 发送与 handler

- `arch_ipi_broadcast(vector, exclude_self)`（`kernel/arch/aarch64/intr/ipi.c` 新增）：`vector == IPI_VECTOR_TLB` → `gic_send_sgi(gic_dev_current(), 3, 目标掩码, filter=all-but-self)`（filter/target 编码沿用 `gic_send_sgi` 现有常量，与 `ipi_test.c` 用法一致）；其他 vector 暂 panic。
- TLB handler 注册到 SGI 3：`arch_local_irq_save` → `arch_flush_tlb_all()`（`arch/mmu.h:144` 已有）→ ack（§6.4）→ 清 wanted → restore。
- `kernel/memory/tlb.c` 编入 aarch64 build（`kernel/Makefile` 白名单加 `memory/tlb.c`）。

### 6.4 shootdown 协议（v3 重写，修 #8）

现有单 `tlb_wanted/tlb_ack` 槽与静默超时不可用。M3 改为：

1. **发起方串行化**：`tlb_shootdown` 全程持全局 `tlb_sd_lock`（`spin_lock_irqsave`）——同一时刻至多一个发起者，ack 槽无并发覆写。
2. **代数式 ack**：per-CPU `tlb_ack_gen`（monotonic 计数）。发起者记录发起时 `target_gen = percpu[i].tlb_ack_gen + 1`，等待 `tlb_ack_gen >= target_gen`。handler 每次执行 `tlb_ack_gen++`。历史 ack 不会误判为本次。
3. **超时 FATAL**：等待上限（如 5 s，经 jiffies/counter）未达成 → `panic("TLB shootdown: CPU%u ack timeout gen=%u")`。**不再静默继续**。
4. x86 端同步迁移到该协议（x86 `tlb.c` 同文件改造；`ipi_broadcast` x86 实现已存在）。x86 `systest_repeat` 回归验证。

死锁论证：`tlb_sd_lock` 由发起者持有，handler 不取该锁（只写自己的 `tlb_ack_gen`）；发起者等 ack 期间目标 CPU 只需跑 handler。与 `pt_lock` 的交互：`pt_lock` 持有者内部调用 `shootdown`（取 `tlb_sd_lock`）；锁序固定 `pt_lock → tlb_sd_lock`，无反序路径（handler 两把都不取）。

---

## 7. vmm_init 双后端（v3 补调用点）

### 7.1 x86_64

行为不变（PMM walk + kernel_map 硬编码 + shootdown），迁入 x86 backend。

### 7.2 aarch64（v3 明确生产调用点）

**调用点**：`kernel/arch/aarch64/boot/main.c` 中 `arch_boot_direct_map_init()` 成功返回后**立即**调用：

```c
if (arch_boot_direct_map_init()) { /* 现有失败路径 */ }
int rc = arch_vmm_init();          /* 新增 */
if (rc) { log_err("FATAL: arch_vmm_init rc=%d\n", rc); arch_cpu_halt(); }
```

`arch_vmm_init` 实现：读 `aarch64_read_ttbr1()`，校验 PA 非零/对齐/< 1 TiB，`kernel_map = pa + ARCH_PAGE_OFFSET`。启动 selftest 断言 `kernel_map == (ttbr1 & BASE_MASK) + ARCH_PAGE_OFFSET`（即指向当前活跃 root）。

---

## 8. 测试与验收（v3 修正）

测试落 **`hosttests/cases/`**（实际 harness 目录）；QEMU 用 `KERNEL_SELFTEST=1` 与**非 selftest 正式镜像**两种。

### 8.1 M2 矩阵

| 阶段 | 测试 | 期望 |
|----|----|----|
| RED | `hosttests/cases/` slab 跨 16 size alloc/free/复用（新 case） | 改动前 fail |
| GREEN | 同上 | x86_64 host pass |
| PREFLIGHT | arena 含 slab 段后 preflight 检查（连续 RAM / < 2 GiB / 不重叠 / 容量）注入坏布局 | FATAL 路径命中 |
| QEMU | `KERNEL_SELFTEST=1` aarch64 -smp 2 启动 | 到 main；`slab_init` 断言过；双重计数抽查位为 0 后被 slab_init 置 1 |
| 回归 | x86 hosttests + `systest_repeat.py` 5 连 | 5/5 |

### 8.2 M3 矩阵

| 阶段 | 测试 | 期望 |
|----|----|----|
| RED | `hosttests`：12 合法组合 × leaf/block + 4 拒绝组合（`DEVICE\|EXEC` × {RO,RW} × {USER,KERNEL}）+ 软件位 round-trip | fail → GREEN |
| RED | `hosttests`：split 后 512 PTE 逐条验 PA/权限/软件位；split 分配失败注入（mock `alloc_4k_page` 返回 0）验证**原映射完好** | fail → GREEN |
| RED | `hosttests`：PROT_NONE 三态（query EPROT_NONE+PA / unmap 返回 PA 不 free / set_software 转换） | fail → GREEN |
| RED | `hosttests`：`arch_vmm_init` 后 kernel_map == TTBR1 root | fail → GREEN |
| QEMU 单核 selftest | map/update/unmap/query/set_software × 4k/2m + split 单元 | 全过 |
| QEMU 多核 selftest | **boot command 工作项**跨核测试（§0 #14 流程：B 记录旧映射读数 → A 替换/解除并等 ack → B 复读验证 → B 置结果 → A 检查）；双 CPU 并发竞争同一 2 MiB 段 split；-smp 2 与 -smp 4 | 全过，无死锁，无超时 |
| QEMU **正式镜像** | 非 selftest 构建 -smp 2：启动后触发一次运行期 map+shootdown（如通过既有调试通道或启动期自检调用），验证 AP ack | 完成不挂死 |
| 回归 | x86 `systest_repeat.py` 5 连；M1 16 组矩阵不回归 | 5/5 |

### 8.3 验收门槛

- **M2**：hosttest GREEN + preflight FATAL 路径验证 + aarch64 selftest QEMU 启动 + x86 5/5。
- **M3**：hosttest 全 GREEN + aarch64 单核/多核 selftest + **正式镜像双核 shootdown** + x86 5/5。
- 不要求：aarch64 shell / aarch64 systest_repeat（M4+）。

---

## 9. 风险与遗留

### 9.1 风险（v3 修 R7，删与现状相同的假对策）

**R7（v3 重写）**：持 `pt_lock` 等待 shootdown ack 的死锁风险。**对策**：(a) `pt_lock` 一律 `spin_lock_irqsave`；(b) TLB IPI handler 不取任何锁；(c) 锁序 `pt_lock → tlb_sd_lock` 全序固定；(d) §8.2 双 CPU 竞争同段测试验证。

**R8**：SGI handler 与 GIC 中断嵌套。**对策**：handler 内 irqsave；GIC 优先级配置不动；多核测试覆盖。

**R9**：caller 排除执行不严。**对策**：aarch64 链接后 `nm` 检查无 `vma_*`/`uaccess_*`/fork 相关符号。

**R10（v3 新增）**：AP 开 IRQ 后正式构建行为变化（原假设 AP 永久静默）。**对策**：正式镜像双核验收 + IPI 自测回归（ipi_test 假设仍成立）。

其余 R1-R6 同 v2（布局/性能/L3 不回收/软件位真机/行为分裂注释）。

### 9.2 Follow-up

F1 merge_4k_to_2m；F2 per-VA shootdown；F4 slab 递归 flag；F5 `vma.c`/`uaccess.c`/`task.c` aarch64 重写（M4）；F6 M4 用户地址空间；F7 `arch_vmm_free_user_map`（M4）；F8 x86 表页迁 `alloc_4k_page` 评估。

---

## 10. 实施拆分（v3 调序）

1. **M2.1**：slab 锁替换 + `slab_layout_compute()`；hosttest RED→GREEN。
2. **M2.2**：arena preflight 扩 slab 段（容量/M0/重叠检查前置）+ boot reservation 排除 slab 段 + 双重计数断言；aarch64 编入 slab.c、删 stub；QEMU 验证。
3. **M3.1**：SGI 3 白名单 + `arch_ipi_broadcast` + TLB handler + 正式构建 AP IRQ 使能 + `tlb.c` 编入 + shootdown 串行化/代数 ack/超时 FATAL（x86 同步迁移）；QEMU 多核（含**正式镜像**）IPI 验证。
4. **M3.2**：公共 vmm.h 语义层 + x86 `pte.h` 私有层拆分（PAGE_* 迁移，x86-only 文件改 include）；x86 回归全过。
5. **M3.3**：aarch64 原语扩展（软件位进/出 + EPROT_NONE + block 编解码）+ backend 4 KiB 全套（map_new/update/set_software/unmap/query）+ `pt_lock`；hosttest。
6. **M3.4**：block map/unmap + split（先建后拆）；hosttest + 单核 QEMU。
7. **M3.5**：`arch_vmm_init` 生产调用点 + 启动断言；多核跨核工作项测试；正式镜像双核验收。
8. **M3.6**：总回归——x86 5/5、aarch64 单/多核、M1 矩阵、`nm` 符号检查。

每步独立 RED→GREEN；失败不进下一步。M3.1 提前到 M3.2 之前：shootdown 是后续所有映射操作的验收依赖。
