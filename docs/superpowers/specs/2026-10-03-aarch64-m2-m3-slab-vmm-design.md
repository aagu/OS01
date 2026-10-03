---
title: OS01 aarch64 M2 Slab + M3 运行期 VMM 设计 (v8)
created: 2026-10-03
type: spec
status: revised-for-review
tags: [osdev, aarch64, memory, slab, vmm, tlb, ipi]
supersedes: f8166b4 (v7), f8d088c (v6), 8636672 (v5), 9311ae9 (v4), 1879a43 (v3), 8626553 (v2), 93f8ae7 (v1)
---

# aarch64 M2 Slab + M3 运行期 VMM (v8)

> v8 修订 v7 评审的 7 项问题（1 P0 + 4 P1 + 2 P2，其中 #7 是 v7 自身的口径矛盾）。核心修正：**x86 迁移补全 `ipi_ready` 发布与 mask 发送**（v7 只写了 aarch64，x86 目标集会恒空）；**未就绪 AP 的 TLB 语义收口**（禁止在 root 使用者未全就绪时变更共享映射）；**root 发布登记制**（`root_is_published` 落到真实状态源——M1 装 TTBR1 时就登记，不再宣称"M3 只有一个发布点"）；**逻辑 CPU ↔ GIC target bit 映射**写成平台假设并启动校验；**工作项等待有界化**（seq 首项 = 1）；持锁 shootdown 论证改为通用规则。v7 建立的其余契约保留。本文自包含。

> v7 及更早的修订明细见 git 历史（v7 = f8166b4）。

---

## 1. 意图、基线与完成标准

依据 roadmap P2：M2 让 aarch64 kernel 从 `kmalloc()=NULL` 走到任意缓存大小可分配/释放；M3 让运行期可映射/解除映射 4 KiB、创建/拆分 2 MiB block，并保留 x86_64 全部现行行为。M2+M3 是 M4 用户地址空间的前置；M4 不在本 spec。

**基线**：master @ `dabc9f0`。前置：M0 ✅ / M1 ✅ / Generic Timer ✅ / GICv2 Phase 1 ✅ / IPI fix ✅ / PMM arch-neutral ✅。

### 1.1 aarch64 启动顺序实测

`kernel/arch/aarch64/boot/main.c`：① M1 preflight / `aarch64_early_arena_init()`（≈:388-405，`pmm_init` 之前）→ ② `pmm_init(handoff)`（:411，内部 `pmm.c:317` 调 `slab_init()`）→ ③ `arch_boot_direct_map_init()`（:422）。**真实 Slab 运行在 M0 直映（TTBR0，0..2 GiB）下**——slab 元数据与 8 个预分配 2 MiB 页的物理地址必须 < 2 GiB 且被正确记账（§3.2）。

### 1.2 M2 完成标准

- aarch64 build 编入 `kernel/memory/slab.c`，删 `kernel/arch/aarch64/runtime/slab_stub.c`。
- `slab.c` 锁路径换 `arch_local_irq_save/restore`（§3.1）；文件内不再出现 `pushfq/cli/sti/popfq` 字面。
- **布局与记账契约**（§3.2-3.3）：preflight 在 `pmm_init` 之前完成全部检查；slab 帧恰好记账一次；启动断言实际 `end_of_struct ≤ 预估上界`。
- aarch64 `KERNEL_SELFTEST=1` QEMU 启动到 main 不 panic；selftest 跨 16 缓存大小 kmalloc/kfree pass。
- x86_64 hosttests + `systest_repeat.py` 5 连 pass（slab 行为不变）。

### 1.3 M3 完成标准

- **IPI/shootdown 契约**（§6）：SGI 3 = TLB；正式构建 AP 使能 IRQ 并持续服务工作项；`tlb_shootdown` 串行化 + 代数 ack + 超时 FATAL；`kernel/memory/tlb.c` 编入 aarch64。
- **vmm 语义层**（§4）：公共 `vmm.h` 只含 `VM_*` 与语义 API；x86 硬件位迁 `arch/x86_64/pte.h` 私有头；`vma.c`/`uaccess.c`/`task.c` 保持不编入 aarch64（当前 gate 已排除，写死契约）。
- **替换协议**（§4.4.3/§5.3）：4 KiB translation 变化的更新按类别分协议（BBM 或原子 store）；block↔table 替换仅支持**未发布 root**（单次原子 store，无失效需求）；**已发布 root split 返回 `-EPERM`**（等级感知实现属 F10）；split 失败不留空洞。
- **所有权**（§4.4.4）：backend unmap 永不 free。
- aarch64 `aarch64_pt_*` 受锁公开原语（§5）+ 软件位 + `AARCH64_PT_EPROT_NONE`。
- `arch_vmm_init` 有生产调用点（§7.2）+ 生产 shootdown 探针（§7.3）。
- host 边界测试（12 合法 + 4 拒绝 + split + 软件位 + PROT_NONE）+ QEMU 单核/多核 selftest + **正式镜像双核探针**。
- x86_64 `systest_repeat.py` 5 连 pass。

### 1.4 不属于本 spec

M4 用户地址空间（user PGD / EL0 切换 / uaccess 故障恢复 / `arch_vmm_free_user_map`）；`merge_4k_to_2m`（F1）；per-VA shootdown（F2）；`vma.c`/`uaccess.c`/`task.c` aarch64 重写（M4）；Slab 算法优化；ASLR。

---

## 2. 已核实的约束

1. `slab.c:36-55` 锁路径 inline `pushfq; cli` / `sti`；`arch_local_irq_save/restore` 已存在（`kernel/include/arch/irq.h`）。
2. `slab_init`（`slab.c:336-415`）：在 `PMMngr.end_of_struct` 后为 16 个 cache 写 `struct Slab` + `10×long` + `color_length` bitmap（32 B 缓存的 bitmap 8 KiB，16 组含对齐约 16.4 KiB）；然后以 j-loop（`j 从 align2M(旧 end_of_struct)>>21 到 新 end_of_struct>>21`）**无条件**标记覆盖的 2 MiB 帧；`slab_page_start = align_up(end_of_struct, 2M)` 后按地址直接占 8 个 2 MiB 帧（cache 0-7；cache 8-15 首次 kmalloc 经 `alloc_pages` 取页），同样无条件记账。BSS 静态变量不占 arena。
3. PMM bitmap 与 boot reservation 均为 **2 MiB frame 粒度**；`pmm_reserve_boot_ranges`（`pmm.c:671`）幂等（已置位跳过，但**不减计数**——它自己 ++/-- 一次）。
4. GIC 白名单（`gic.c:25-30`）：SGI 0/1/2（ipi_test）+ CNTP PPI；**SGI 3 空闲**。
5. `secondary_idle`（`smp.c:244-263`）：AP 处理一次 boot command 后，仅 `OS01_SELFTEST` 开 DAIF.I，随后 `halt` 永循环。
6. `spin_lock_irqsave`（aarch64 `spinlock.h:83`）：**先 `irq_save` 再自旋**——等待者 IRQ 关闭。
7. `tlb_shootdown`（`tlb.c:22-65`）：单 wanted/ack 槽；超时打印后**继续执行**；调 `ipi_broadcast`（:38）；未编入 aarch64。
8. `arch_flush_tlb_all/page` aarch64 实现已存在（`arch/mmu.h:144`，`tlbi vmalle1/vae1 + dsb sy + isb`）。
9. `aarch64_pt_map_4k` 遇有效 PTE 返回 `EEXIST`；`query_4k` 遇 `Valid=0` 返回 `ENOENT`；`encode_perm`（`page_table.c:238`）拒绝 `PERM_ALL_BITS` 外的位。
10. `walk_to_l3`/`encode_perm`/`tlb_invalidate_local` 均为 page_table.c 私有（`:110` forward decls）；该文件自述"独占描述符编码"。
11. `percpu_t` 钉死 144 B（`percpu.h:22` `PERCPU_DATA_SIZE` + `_Static_assert` + head.S stride 站点）。
12. `early_arena.c:331-332` 发布 `table_base_pa`/`table_end_pa`，现固定 `table_base = PMM 元数据末尾`；被 runtime_tree builder / validator / arena 结构共用。
13. 直接操作 x86 硬件 PTE 的文件：`memory/vma.c`、`memory/uaccess.c`、`sched/task.c`（fork）、x86 `do_page_fault` 路径、`arch/x86_64/memory/boot_direct_map.c`。aarch64 source gate（`kernel/Makefile:39-47`）白名单式，均未编入。
14. x86 `vmm_unmap_4k_page`（`vmm.c:317`）会释放物理页并做 COW 引用计数——释放逻辑属 x86 层，不进 backend（§4.5）。
15. aarch64 `log_err`（`boot_log.h:14`）只收单字符串；`kputs`/`kputu` 可用。

---

## 3. M2：slab 接入与物理布局/记账契约

### 3.1 锁路径替换

`slab.c` 顶部加 `#include <arch/irq.h>`；`slab_lock_acquire` 返回类型 `uint64_t` → `arch_irq_state_t`，首句换 `arch_irq_state_t flags = arch_local_irq_save();`；`slab_lock_release` 形参同步换型，末尾 `if (flags & (1UL<<9)) sti` 换 `arch_local_irq_restore(flags);`。其余不动。

### 3.2 frame 级记账契约（v4 核心）

**单一事实来源**：新增纯函数 `slab_layout_compute()`（`kernel/include/memory/slab.h`），从 `slab_init` 地址推进规则精确推导：

```c
struct slab_layout { uint64_t meta_bytes; uint64_t reserved_2m_pages; };
/* meta_bytes = Σ_{i=0..15} [ sizeof(struct Slab) + 10*sizeof(long)
 *   + color_length(size_i) + 10*sizeof(long) ]，long 对齐；
 * color_length(s) = align8(PAGE_2M/s / 8)。reserved_2m_pages = 8。
 * 只读 kmalloc_cache_size[].size 与类型布局，不触碰内存。 */
```

**arena 公式链**（`early_arena` 布局计算内，全部溢出检查）：

```
pmm_meta_end   = arena_start + layout.end_of_struct_off   /* 实际 end_of_struct，
                                                             align_down 语义（pmm_boot.h:22）；
                                                             与 slab_init 写入起点一致 */
slab_meta_end  = pmm_meta_end + slab_layout.meta_bytes
slab_page_start= align_up_2M(slab_meta_end)
slab_page_end  = slab_page_start + 8 * 2 MiB
table_base_pa  = slab_page_end                              /* 已 2M 对齐 */
table_end_pa   = table_base_pa + table_pages * 4 KiB        /* 现有表页数计算 */
arena_end      = align_up_2M(table_end_pa)
```

注意：**不用 `total_bytes`**（它是 `align_up_4k(end_of_struct_off)`，与 align_down 的 `end_of_struct_off` 在边界处差最多 4 KiB，用错会让 slab 页起点偏移一个 2 MiB frame）。`total_bytes - end_of_struct_off` 的对齐余量单列独立检查：确认被 arena 的 2 MiB 对齐吸收（`slab_meta_end` 起算已含此余量时断言通过，否则 FATAL）。

**记账规则（每 2 MiB frame 恰好一条路径）**：

| frame 范围 | 记账者 | 说明 |
|----|----|----|
| [arena_start, arena_end) **全部** | boot reservation（`pmm_reserve_boot_ranges`，幂等） | **含 slab 段**。边界 frame（PMM/slab 元数据共享、slab 段与表页池邻接）由 reservation 统一记一次 |
| slab_init 的 j-loop 与 8 页循环 | **幂等化**：`bits_map` 位已置 → 跳过（不 ++/--） | x86 无预预留 → 位全 0 → 全标记，**行为不变**；aarch64 已被 reservation 标记 → 跳过 |

这消除了 v3 "按字节区间排除"的不可能性：共享边界 frame 无需切分，reservation 覆盖一切，slab_init 幂等去重。`slab_init` 幂等化是共享代码改动，但 x86 语义不变（hosttest 验证 using/free 计数与改动前一致）。

**preflight 检查（`pmm_init` 之前，写入内存前）**：① arena 扩展后仍落连续 RAM；② `arena_end ≤ 2 GiB`（M0 覆盖），越界 FATAL（打印需求/可用）；③ 公式链各步无回退/溢出；④ 运行期断言 `slab_layout_compute().meta_bytes` 与 preflight 一致。

**启动后验证**：`slab_init` 末尾断言实际 `end_of_struct ≤ slab_meta_end`；host test 用**实际 j-loop 起止公式**（`j 从 align2M(旧end)>>21 到 新end>>21`，及 8 页循环）证明每个被 slab_init 触碰的 frame 都在 reservation 范围内（即被跳过、不再二次计数），且 reservation 外无 slab frame。

### 3.3 M1 表页池移位的同步修改（v4 新增）

`table_base_pa` 从"PMM 元数据末尾"改到"slab 页之后"，同步修改：

1. `early_arena.c` arena 布局计算（§3.2 公式链）+ 容量选择（arena 总长变大，可能影响能容纳它的 RAM 区间选择）；
2. `runtime_tree.c` builder：从新 `table_base_pa` 起取页（该文件只消费 base/end，预计零改动，需验证）；
3. M1 validator：16 组矩阵重跑（arena 尺寸变化会改变期望值）；
4. host tests：arena sizing / 稀疏 / 容量耗尽 / 坏 root 注入用例更新期望；
5. 失败注入：slab 段推出 2 GiB 边界的注入用例（期望 FATAL）。

### 3.4 aarch64 build 接入

`kernel/Makefile:39-47` 白名单加 `memory/slab.c`；删 `slab_stub.c`。

---

## 4. vmm 语义层与接口

### 4.1 两层分离

- **公共语义层**（`kernel/include/memory/vmm.h`）：`VM_*` + 语义 API；通用代码只见此层。
- **x86 硬件层**（`kernel/include/arch/x86_64/pte.h`，新建）：现有 `PAGE_*` 位 + `vmm_pt_walk`（裸 PTE 指针）+ `vmm_free_user_map` 迁入；仅 x86-only 文件 include（§4.5 列表）。

### 4.2 语义位

```c
#define VM_PRESENT   /* 有效映射 */
#define VM_WRITE     /* 可写 */
#define VM_USER      /* EL0 可达 */
#define VM_NO_EXEC   /* 不可执行；缺省（不含此位）= 可执行 */
#define VM_HUGE      /* block 描述符 */
#define VM_NOCACHE   /* 设备/UC；含此位必须含 VM_NO_EXEC */
#define VM_PROTNONE  /* 软件位：无效但持有 PA */
#define VM_COW       /* 软件位：COW 共享，写触发 fault */

#define VM_KERNEL_RW (VM_PRESENT | VM_WRITE)
#define VM_KERNEL_RO (VM_PRESENT)
#define VM_USER_RW   (VM_PRESENT | VM_WRITE | VM_USER)
#define VM_USER_RO   (VM_PRESENT | VM_USER)
#define VM_DEVICE    (VM_PRESENT | VM_WRITE | VM_NOCACHE | VM_NO_EXEC)
```

位位置由 `arch/<arch>/vmm_backend.h` 定义。**`VM_PRESENT=0 且 VM_PROTNONE=1`** 为合法"无效但持有 PA"状态。**拒绝组合**：`VM_NOCACHE` 而无 `VM_NO_EXEC`（× {RO,RW} × {USER,KERNEL} = 4 case，backend 返回 `-EINVAL`；aarch64 侧 NOCACHE 强制 PXN|UXN）。

### 4.3 语义 API（唯一一套；无 set_software）

```c
int   arch_vmm_init(void);
int   arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
      /* 遇任何已占用（含 PROT_NONE）返回 -EEXIST，不覆盖 */
int   arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                         uint32_t vm_flags, uint64_t *old_phys_out, uint32_t *old_vm_out);
      /* 写完整目标语义状态（含软件位），按 §4.4.3 分类协议执行 */
int   arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt,
                        uint64_t *phys_out, uint32_t *old_vm_out);
int   arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt,
                        uint64_t *phys_out, uint32_t *vm_out);
      /* 三态：0（有效）/ -EPROT_NONE（无效但持有 PA，phys_out 有效）/ -ENOENT */
int   arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);
```

COW/PROT_NONE 都经 `update_4k` 表达（输入完整目标状态）：COW 标记 = update(同 PA、清 W、置 VM_COW)；PROT_NONE 暂存 = update(VM_PROTNONE)；恢复 = update(VMA 层持有的原 prot——与 x86 现状同构，原权限在 `vma->vm_page_prot`，不在 PTE）。

### 4.4 后端行为契约

#### 4.4.1 aarch64 原语层（page_table.c 受锁公开原语）

backend 不写裸描述符。page_table.c 新增/扩展（内部持 `pt_lock_for(root, l2)`，见 §5.4，并做 local TLBI）：

```c
int aarch64_pt_map_4k_ext (root, va, pa, perm, uint64_t software_bits);   /* EEXIST */
int aarch64_pt_replace_4k (root, va, pa, perm, uint64_t software_bits,
                           uint64_t *old_pa, uint64_t *old_sw);           /* §4.4.3 分类 */
int aarch64_pt_query_4k   (root, va, *pa, *perm, *sw);                    /* +EPROT_NONE */
int aarch64_pt_unmap_4k   (root, va, *pa, *perm, *sw);                    /* +EPROT_NONE */
int aarch64_pt_map_2m_block / unmap_2m_block / split_block_2m (…);        /* §5 */
```

软件位编码：x86 = bit 9（PROTNONE）/ bit 10（COW）；aarch64 = bit 55 / bit 56（描述符保留区）。`AARCH64_PT_SOFTWARE_PROTNONE/COW` 常量 + `AARCH64_PT_EPROT_NONE` 返回码新增。TLBI 分层：**page_table.c = per-VA local TLBI（及时性）；`tlb_shootdown` = 跨核 + 本地全表失效（协议性，见 §6.4 步骤 3）**。两层都刷本地是**有意为之**：本地重复失效幂等无害，page_table.c 的 per-VA TLBI 保证原语返回时本核已不可命中旧翻译，不必依赖 caller 是否调 shootdown。

#### 4.4.2 x86_64

表页保持 `calloc`/`kfree`；`vmm_init` PMM walk、`kernel_map = Phy_To_Virt(0x101000)`、覆盖语义 wrapper 全部不变，迁入 x86 backend/私有头。

#### 4.4.3 替换协议（按变化类别，v4 分类）

设 D_old（PA_old、权限、内存类型、软件位）→ D_new：

| 类别 | 协议 |
|----|----|
| 仅 AP/XN 权限位变（同 PA、同内存类型、同有效性） | 原子 8 B store → `dsb ishst` → local TLBI → shootdown |
| **内存类型变（AttrIndx / NOCACHE）** | PTE 级 break-before-make（下述序列） |
| PA 变（重映射） | PTE 级 break-before-make |
| 有效性翻转（↔PROTNONE） | PTE 级 break-before-make |

PTE 级 BBM 序列（持 `pt_lock_for(root, l2)`）：`*pte = 0 → dsb ishst → tlbi vae1 → dsb ish → shootdown → *pte = D_new → dsb ishst → tlbi vae1 → dsb ish; isb`。**窗口内该 4 KiB 页不可翻译**——caller 必须拥有并静默该范围（M3 内仅探针 scratch 页满足；契约写入 `update_4k` 文档注释）。block↔table 替换不走此序列（§5.3：未发布 root 原子 store / 已发布 root `-EPERM`）。

**内存类型别名约束（v6 新增）**：`VM_NOCACHE` 映射的合法 PA 范围 = **未被 Normal direct map 覆盖的物理地址**（设备 MMIO 窗口或 PA 空洞）。禁止对 RAM PA（已有 Normal direct map 别名）建 NOCACHE 映射——同一 PA 的 Normal/Device 别名是属性不兼容别名，BBM 只处理被更新的 VA，消除不了另一条别名（Arm 内存模型要求避免）。此为 caller 契约，写入 backend 文档注释；DEBUG 构建可加 PMM zone 交叉检查。**内存类型变更的测试一律为纯描述符级（无 live 访问）**：语义位只有 Normal 与一种 NOCACHE，任何"X↔X"组合都不构成真实的类型变更，live 的 Normal↔Device 切换需要先设计无不兼容别名的物理区域与切换流程（推迟，见 F11）。

每类独立测试（§8.2），含跨核可见性。

#### 4.4.4 所有权

**backend 永不释放数据页**：`unmap_4k`/`unmap_2m`/`update_4k` 只摘除/替换并返回旧 PA + 旧软件位；`free_4k_page`/`free_pages`/COW 引用计数由拥有该页的上层执行（M4 前仅 x86 vma/uaccess/fork 与 aarch64 测试代码）。

#### 4.4.5 `unmap_2m` 契约

`pmd[l2]` 是 block（V=1, bit1=0）→ `*phys_out = desc & PA_MASK`，清条目，`dsb ishst` → block local TLBI → shootdown，不释放数据页，L2 表页保留；是 L3 table 描述符 → `-EINVAL`（类型错误）；无效 → `-ENOENT`。

### 4.5 caller 范围与 x86 wrapper

**M3 保持不编入 aarch64**（gate 白名单已排除，零改动写死契约）：`memory/vma.c`、`memory/uaccess.c`、`sched/task.c`（整文件——构建系统只有文件粒度；M4 需非 fork 功能先拆文件）。

**x86 旧 wrapper**（`vmm_map_4k_page`/`vmm_unmap_4k_page` 等）= backend 摘除调用 + **原释放/COW 逻辑原样保留在 x86 层**（`vmm.c:317` 的 `free_4k_page`/`page_cow_put` 不动）；fork/COW/munmap 回归验证行为不变。

**include `arch/x86_64/pte.h` 的文件**（保留裸 PTE 访问）：`vma.c`、`uaccess.c`、`task.c`、x86 `do_page_fault` 路径、`arch/x86_64/memory/boot_direct_map.c`、`kernel/memory/vmm.c`（x86 部分）。公共 `vmm.h` 的 `PAGE_*` 全部删除，M3 不迁移上述文件内部实现。

---

## 5. aarch64 block 与 split

### 5.1 block 描述符编码

`AARCH64_PT_DESC_VALID=0x001`（bit 0）；block 类型 **bit 1 = 0**（table/leaf = 1）；**block 输出地址在 bits [39:21]**（L2 block 描述符的 OA 字段是 [47:21]，IPS=40 → 有效 PA 位 [39:21]；低位 [20:12] 对 block 是 RES0，非地址字段——与 4 KiB leaf 的 [39:12] 不同，勿混）；AP[2:1] bits[7:6]（`KERNEL_RW=0x0/USER_RW=0x40/KERNEL_RO=0x80/USER_RO=0xC0`）、SH bits[9:8]（Normal=inner-shareable 0x300 / Device=0x0）、AttrIndx bits[4:2]（Normal=0x4 / Device=0x0）、AF bit 10、PXN bit 53、UXN bit 54——与 leaf 同编码；软件位 bit 55/56。新增 `encode_block_desc()`（独立于 leaf 专用 `encode_perm`，共用 vm→AP/SH/Attr/XN 翻译）。

### 5.2 map/unmap 2 MiB block

**map**：校验 root/va/pa 对齐与 < 1 TiB；持 `pt_lock_for(root, l2)`；经 §5.2b `walk_to_l2(create)` 走到 `pmd`；`pmd[l2]` 已占用 → `-EEXIST`；写 block 描述符（`dsb ishst` 前后）；root 已发布时 block local TLBI + shootdown；解锁。
**unmap**：§4.4.5。

### 5.2b `walk_to_l2(create)` 契约（v6 新增：上级表创建路径）

新 VA 可能连 L0/L1 子表都没有。`map_2m` / `map_4k_new` 到达目标 L2 slot 的路径：

1. **锁序**：先取 `pt_lock_for(root, l2)`（L2 slot 级操作），再取 `pt_upper_lock`（L0/L1 创建段）——与 §5.4 全序一致。
2. **逐级 ensure**（`pt_upper_lock` 内）：读 slot → 若空：`alloc_4k_page()` → `zero_page()` → 发布最小表描述符（V|TYPE|PA）→ `dsb ishst`。L0 与 L1 两级均可能创建。
3. **`-ENOMEM` 回滚**：任一级 alloc 失败 → 释放（若有）本次未发布页 → 解锁两锁 → 返回 `-ENOMEM`。**先前已创建并发布的空中间表保留**——它们无害（全零槽位）、可被后续调用复用，不构成功能泄漏（M3 无中间表回收，与 F1 同属一类已知代价）。
4. **发布原子性**：每级单次 8 B store 发布；未发布 root 无 TLBI；已发布 root 每级发布后 local TLBI（当前 M3 caller 的目标 VA 在空洞，实际不会走到已发布 root 创建路径，但契约按完整语义写全）。
5. **测试**：在全新 L1 范围（L0 slot 空）创建 block——L0/L1 两级 ensure 都触发，验证映射可 query、ENOMEM 注入时中间表保留且可重试成功。

### 5.3 split 协议（v7：未发布 root 原子 store；已发布 root = -EPERM）

**范围收敛依据**（v5 等效替换方案作废）：Arm 对 block↔table 描述符互换的免 BBM 许可取决于 `ID_AA64MMFR2_EL1.BBM` 报告的**支持等级**（0/1/2 表示处理器对 BBM 要求的放宽程度——注意这是 CPU 特性等级，**不是**翻译表层级），且**发布后仍需跨核失效**（其他核缓存的旧 block 项不会被本核 TLBI 清除，可能产生 TLB conflict）。具体等级 ↔ 允许操作的映射在 F10 设计时以 Arm ARM 原文核对，本 spec 不断言。cortex-a53 上 QEMU 测试通过不能替代架构前提。M3 无任何 caller 需要已发布 root 的 split（探针 scratch VA 的 L2 slot 本为空），故：

**root 生命周期契约（v8 重写：登记制状态源）**：`root_is_published()` 必须有真实状态来源，不能只靠 caller 口头保证。设计**发布登记表**：

```c
/* kernel/arch/aarch64/memory/page_table.c 内 */
static uint64_t published_roots[AARCH64_PT_MAX_PUBLISHED_ROOTS];  /* PA，静态零 */
static spinlock_T published_roots_lock = { .lock = 1UL };
bool aarch64_pt_root_publish(uint64_t root_pa);   /* 登记；重复登记幂等 */
bool aarch64_pt_root_is_published(const uint64_t *root);
```

- **登记点 = 安装点**，在首次可能被任何 CPU 使用之前调用：**M1 安装 TTBR1 root 时**（`boot_direct_map.c:85` 一带——早于 `arch_vmm_init`，v7 称"M3 只有 arch_vmm_init 一个发布点"不实）即登记；`arch_vmm_init` 引用同一 root（幂等）；M4 的 `arch_switch_mm` 成为用户 root 的登记点。容量上限（如 8）超限 panic。
- `split_block_2m` 开头查登记表：在表 = 已发布 → `-EPERM`；不在表 = 未发布，caller 须独占持有（caller 契约，对内核内部 scratch 树天然成立）。本核 `is_active_root()` 保留为 DEBUG 辅助断言。
- 查表在 `pt_lock` 外做（登记表有自己的锁；登记只发生在启动/M4 切换路径，与 split 无实际竞争）。
- 已发布 root 的等级感知 split（BBM 支持等级 + 发布后全范围跨核失效与完成等待）列为 **F10**。

```
/* 未发布 root 的 split */
 0. if (root_is_published(root)) return -EPERM;   /* 生命周期契约 + 本核 is_active_root 辅助 */
 1. l3_pa = alloc_4k_page();  if (!l3_pa) return -ENOMEM;   /* 映射未动 */
 2. zero_page(l3_pa);
 3. lock(pt_lock_for(root, l2));
 4. d = pmd[l2];
    if (!(d & VALID))            { unlock; free_4k_page(l3_pa); return -ENOENT; }
    if (d & TYPE_TABLE)           { unlock; free_4k_page(l3_pa); return -EAGAIN; } /* 并发 split，caller 重试 */
 5. for i in 0..511:
        pte[i] = (block_pa + i*4K) | inherit(d);   /* 全属性 + bit55/56 继承 */
    dsb ishst;
 6. pmd[l2] = l3_pa | encode_table_desc;    /* 单次原子 8 B store */
 7. dsb ishst;
 8. unlock(pt_lock);
```

未发布 root 无 CPU 可能缓存其翻译——无 BBM、无 TLBI 需求。失败点仅在步骤 1（释放 l3_pa，原映射未动）与步骤 4（释放 l3_pa，无副作用）；步骤 5 后无分配。**测试**：hosttest 512 项 `inherit()` 全等断言（期望 leaf = `encode(block 属性, block_pa + i*4K)`）+ 已发布 root 调用返回 `-EPERM` 断言。v5 的"AP 持续读取待 split 区域"等价性测试随方案作废删除。

### 5.4 锁设计（v5：初始化 + 上级表锁 + 审计门槛）

**存储与初始化**：

```c
/* 静态初始化器：aarch64 spinlock_T 以 1 为未锁（spin_init 置 1，spinlock.h:44），
 * 静态零值会让首次加锁永久自旋 —— 必须显式初始化。 */
static spinlock_T pt_locks[64]    = { [0 ... 63] = { .lock = 1UL } };
static spinlock_T pt_upper_lock   = { .lock = 1UL };
static spinlock_T tlb_sd_lock     = { .lock = 1UL };
/* arch_vmm_init() 内对以上逐个 spin_init() 作双保险。 */
```

`pt_lock_for(root, l2) = &pt_locks[((root_pa >> 12) ^ l2) & 63]`；哈希冲突仅损性能不损正确性。

**覆盖范围（两级）**：

- **L2 slot 级**（`pt_lock_for(root, l2)`）：同一 (root, L2 slot) 的所有结构性操作持锁——`map_4k_ext`/`replace_4k`/`query_4k`/`unmap_4k` 对 `pmd[l2]` 的穿越与 PTE 写、`map_2m_block`/`unmap_2m_block`、`split_block_2m`。query 只读仍持锁：防读到中间态。
- **上级表级**（全局 `pt_upper_lock`）：L0/L1 `ensure_child_table` 的"读空项→分配→写表页→发布"段。**两个不同 L2 slot 各持各的 pt_lock，却可能共享同一个待创建的 L0/L1 条目**——若不加共同锁，并发创建会分配两张表页、后发布者覆盖先发布者（表页泄漏或丢失映射）。创建罕见且临界区短，全局锁足够。

**锁序**：`pt_lock → pt_upper_lock → tlb_sd_lock`（全序，无反序路径；TLB handler 三把都不取）。

**锁类型与死锁论证**（不变式）：

- 三锁均**普通 `spin_lock`**（不用 irqsave——aarch64 `spin_lock_irqsave` 先关 IRQ 再自旋，等待者无法响应 SGI）。等待者 IRQ 保持开。
- **不变式 I1**：三锁永不从中断上下文获取（TLB IPI handler 无锁）。
- **不变式 I2**：vmm 变更 API 入口断言本地 IRQ 开（DEBUG 构建 `BUG_ON(irqs_disabled())`）。
- **持锁 shootdown 通用规则（v8 统一）**：**允许在持有 `pt_lock` 时调用 `tlb_shootdown`**（它只按锁序向下取 `tlb_sd_lock`）。v7 只把"BBM 类 update"列为持锁等 ack 场景是漏算——§5.2 的已发布 root `map_2m`/`unmap_2m` 同样在解锁前 shootdown。安全性论证是**通用**的：任何 CPU 在 `pt_lock` 上自旋时 IRQ 保持开（普通 spin_lock + I2），TLB handler 无锁，故持锁者等待 ack 不会被同一锁的自旋者阻塞。该规则覆盖全部持锁等待路径：BBM 类 `update_4k`、已发布 root 的 `map_2m`/`unmap_2m`/4 KiB 变更后的 shootdown。

**审计门槛（v5 升格为 M3.1 完成门槛，不通过不得进 M3.2）**：枚举全部现存"在 irqsave 锁内调用 vmm 变更 API"的调用链（重点：`slab_lock` 持有路径上的 `vmm_map_page`/`tlb_shootdown`；x86 `pmm_lock` 路径），逐条消除（挪出临界区）或重构（锁拆分）。正式构建无 I2 断言，**审计是唯一防线**——此事实写入 §9 风险。审计清单与结论记录在 M3.1 的验收文档里。

**验证测试**（§8.2）：① 双 CPU 交错——A 持 `pt_lock` 执行 BBM 类 update 等 ack，B 同时竞争同一锁，双方必须完成；② 并发创建共享同一 L1 条目的两个不同 L2 slot 映射，恰建一张 L1 表、无泄漏。

---

## 6. aarch64 生产 SGI / shootdown 契约

### 6.1 SGI 分配

SGI 0/1/2 = ipi_test（已占用）；**SGI 3 = TLB shootdown**（M3 新增，`gic.c:25-30` 每核 banked 白名单加入）。验收含 IPI 自测与 TLB IPI 并存。

### 6.2 正式构建 AP：IRQ 使能 + 持续工作项循环

`secondary_idle` 改为（生产与 selftest 一致）：

```c
/* boot command 处理完后 */
arch_local_irq_enable();  isb;
for (;;) {
    struct ap_work *w = &ap_work[cpu_id()];
    uint32_t state = atomic_load_acquire(&w->state);
    if (state == AP_WORK_READY) run_ap_work(w);
    arch_cpu_pause();   /* 或 wfi：SGI 会唤醒 */
}
```

**工作槽协议（v5 补全：状态机 + 序号）**：

```c
enum { AP_WORK_IDLE = 0, AP_WORK_READY, AP_WORK_DONE };
struct ap_work {
    _Atomic uint32_t state;   /* IDLE→READY→DONE→IDLE */
    uint32_t seq;             /* 每请求递增，区分两次同类请求 */
    uint32_t cmd;             /* WORK_READ64 / WORK_BARRIER / … */
    uint64_t arg0, arg1, out;
};
```

- **BSP（请求方）**：写 `cmd`/`arg*`（普通 store）→ `store_release(state = READY)`（同时发布 `seq = n`，**首项 seq = 1**）→ `load_acquire` 自旋等 `state == DONE && seq == n`，**带 counter 截止时间**（每项约 2 s；超时打印诊断 `WORK-TIMEOUT cpu=%u seq=%u` 并置该项失败状态 → 探针判 FAIL，不无限等）→ 读 `out` → `store_release(state = IDLE)` → 下次请求 `seq = n+1`。
- **AP（执行方）**：`load_acquire` 见 `READY` 且 `seq` 未消费过（**本地"最后已消费 seq"初值 = 0**，与首项 1 错开）→ 执行 → 写 `out`（普通 store）→ `store_release(state = DONE)`。
- **不变式**：`state` 转换只有 BSP 写 IDLE/READY、AP 写 DONE；`seq` 保证 AP 不会把旧 DONE 当新请求、BSP 不会把旧 DONE 当本次结果；AP 卡死由 BSP 侧截止时间兜底（正式镜像探针因此总能打出 FAIL 而不是挂死）。

`ap_work[NR_CPUS]` 固定 per-CPU 数组（BSS，不需要 percpu_t 内嵌——避免再动 `PERCPU_DATA_SIZE`）。此改动移除 `#if OS01_SELFTEST` 门；ipi_test 假设由特殊变普遍，行为兼容。

### 6.2b CPU 在线状态发布协议（v7 重写：双状态）

**现状缺口**：aarch64 从不写 `num_cpus`（x86 SMP 代码专属）；`percpu_init()` memset 清零整个结构且**不设 online**（`percpu.c:15-25`；`main.c:572` 注释称其 populate online 是错的）；`percpu_install_gs(0)+percpu_init(0)` 在 `main.c:578-580`，**晚于** `smp_boot_aps()`（`:501`）；BSP IRQ 使能更晚（GIC init 之后）。后果：`tlb_shootdown()` 永远走 `num_cpus ≤ 1` 本核分支；`slab_lock` 的 `percpu_data[0].online` 门使 aarch64 永久跳锁。

**双状态设计（v7）**——`online` 与 `ipi_ready` 语义拆开，禁止一个字段两义：

| 字段 | 语义 | BSP | AP |
|----|----|----|----|
| `percpu_data[i].online` | percpu 状态已初始化、**锁生效**（x86 兼容：slab_lock 的 `percpu_data[0].online` 门） | `percpu_install_gs(0)+percpu_init(0)` **整体提前到 `smp_boot_aps()` 之前**执行（纯内存初始化无依赖；消灭"AP 已运行而 BSP 未 online"的跳锁窗口），随后 `store_release(online=1)` | `secondary_idle` 中 `percpu_init(cpu)` 后 `store_release(online=1)` |
| `percpu_data[i].ipi_ready`（新字段） | 本核 **SGI 可响应**（IRQ 已开 + handler 已注册） | BSP 自身 IRQ 使能后（GIC init 完成处）`store_release(ipi_ready=1)` | `arch_local_irq_enable(); isb;` 之后、进工作循环前 `store_release(ipi_ready=1)` |

**原子性（v7 补全）**：两字段均为原子字段——写 `store_release`、读 `load_acquire`（目标筛选、探针计数全部）。`num_cpus` 在 `smp_boot_aps()` 前 `store_release` 写入、此后不变；`tlb_shootdown` 内 `load_acquire` 读。

**时序不变式**：

- `slab_init`（`pmm_init:411` 内）早于 BSP percpu init → 全程 online=0 单核跳锁，正确（无 AP 运行）。
- BSP online=1 早于 `smp_boot_aps()` → 任何 AP 开始执行时系统已进入"锁生效"状态，无跳锁窗口。
- shootdown 目标 = `ipi_ready` 集合（不是 online）；AP 先开 IRQ 再标 ipi_ready，发起者看到 ipi_ready=1 的 AP 必能应答 SGI。
- 两字段单次发布、无回退；CPU 下线不在 M3。
- `percpu_t` 一次性增加 `ipi_ready` + `tlb_ack_gen` 两个字段，一次 `PERCPU_DATA_SIZE` bump + head.S stride 更新 + 全量重建。

**测试**：① 探针/selftest 断言 AP1 在 shootdown 目标集（`ipi_ready`）且其 `tlb_ack_gen` 递增（ack 真产生于 AP）；② **一 AP 未 ipi_ready 时发起 shootdown**：只等已就绪者、成功返回、未就绪 AP 的 gen 不变；其就绪后的下一次 shootdown 才递增（无延迟旧 SGI 污染）。

### 6.3 发送与 handler

- aarch64 在 `kernel/arch/aarch64/intr/ipi.c` 实现 **`ipi_broadcast(vector, mask)`**（`mask` = 逻辑 CPU 位图；x86 调用点 `tlb.c:38` 随协议迁移同步改造）：`vector == IPI_VECTOR_TLB` → **`gic_send_sgi(dev, 3, targets=逻辑→GIC 掩码映射(mask), filter=LIST)`**（显式目标列表，与 ipi_test 的 `GICD_SGIR_FILTER_LIST` 用法同款；**不用 all-but-self filter**——它会命中所有其他 CPU 接口，包括尚未 `ipi_ready` 的 AP，延迟投递的旧 SGI 会触发 handler 使 `tlb_ack_gen` 虚增、污染代数判定）。mask 由 `tlb_shootdown` 在 `tlb_sd_lock` 内取快照后传入（§6.4）。其他 vector 暂 panic。
- **逻辑 CPU ↔ GIC target bit 映射（v8 新增）**：GICD_SGIR 的 target 位指向 **GIC CPU interface**，与 `percpu_data[]` 逻辑索引是两种编号。M3 平台假设：QEMU virt 上二者**恒等**（逻辑 cpu N → target bit N）。实施：启动时构建映射表（逻辑 id → GIC target bit，从 per-CPU GIC 配置/MPIDR 亲和推得），并断言恒等；不恒等则 `panic("GIC target mapping non-identity unsupported")`——非恒等拓扑留待 P4 硬件适配。
- TLB handler 注册 SGI 3：`irqsave → arch_flush_tlb_all()（已有）→ ack（§6.4）→ restore`；**无锁**；在 AP online 前、首次 shootdown 前注册。
- `kernel/Makefile` aarch64 白名单加 `memory/tlb.c`。

### 6.4 shootdown 协议（串行化 + 代数 ack + 超时 FATAL）

1. 发起方全程持 `tlb_sd_lock`（**普通 spin_lock**，同 §5.4 契约：仅发起方上下文获取，等待者 IRQ 开）——同一时刻至多一个发起者。
2. **目标快照（v7，锁内取）**：`mask = { i : i != self, load_acquire(percpu_data[i].ipi_ready) }`；只向 mask 内 CPU 发送并等待。快照保证：mask 内 CPU 在快照时刻已可响应 SGI（`ipi_ready` 只置不清），未入 mask 的 CPU 从不收 SGI → 不产生延迟旧 SGI。
3. per-CPU `tlb_ack_gen`（原子 uint32，与 `ipi_ready` 同批加入 `percpu_t` → **一次 `PERCPU_DATA_SIZE` bump + head.S stride 站点 + 全量重建**，`percpu.h:75-85` 流程）。
4. 顺序：
   - handler：`tlbi vmalle1; dsb ish;` `atomic_store_release(&gen, atomic_load(&gen)+1)`。
   - 发起者：本地 `arch_flush_tlb_all()` → 锁内快照 mask → 记 `target_i = load(gen_i)+1`（每个 mask 内 CPU）→ `ipi_broadcast(IPI_VECTOR_TLB, mask)` → 自旋 `while (atomic_load_acquire(&gen_i) != target_i) arch_cpu_pause();`（IRQ 保持开）。**等待条件用 `!=` 而非 `<`**：32 位代数在 `gen = UINT32_MAX`、target 回绕为 0 时 `<` 会立即误判完成；`!=` 在"`tlb_sd_lock` 串行化 + 每 CPU 同时至多一个未完成请求"契约下正确（每 CPU 的 gen 每 shootdown 恰好 +1，等待者追平即停）。回绕单测见 §8.2。
5. 超时（上限如 5 s，经 counter）→ `panic`，**不再静默继续**。
6. **x86 同步迁移（v8 补全 ipi_ready 与 mask 发送）**：x86 启动代码只设 `online`（`smp/boot.c:51`）——若只改 tlb.c，x86 的 `ipi_ready` 目标集恒空。同步修改：
   - x86 BSP 在自身中断使能后、AP 在 trampoline/启动尾部的 IRQ 解屏蔽后发布 `ipi_ready`（`store_release`，与 aarch64 同语义）；
   - `kernel/include/intr/ipi.h` 的 `ipi_broadcast` 签名改为 `(vector, target_mask)`（逻辑 CPU 位图），x86 APIC 实现（IPI per-CPU 发送，掩码即 APIC 目标集）与 aarch64 GIC 实现（§6.3）都按 mask 发送；所有调用点迁移；
   - 测试：**仅指定部分在线 CPU** 的发送（构造 2/4 CPU 中只发部分掩码，验证未含者不收/不 ack、含者 ack）。
   x86 `systest_repeat` 5 连回归验证。
7. **共享映射变更门禁（v8 新增，修"未就绪 AP 缓存旧翻译"漏洞）**：排除未就绪 CPU 出 shootdown 目标集**不能**保证其 TLB 无旧翻译——AP 在 `ipi_ready` 之前已在共享 root（direct map）上执行代码、缓存翻译；若此时变更共享映射，它收不到失效，就绪后也不会自动清除。**M3 契约：任何对已发布 root 的映射变更，必须等待该 root 的全部使用 CPU（= `num_cpus` 内全部 CPU）`ipi_ready` 之后**。实施：vmm 变更入口（DEBUG 断言 + 正式构建门禁）在 `ipi_ready` 计数 < `num_cpus` 时对已发布 root 的变更 panic/fail；M3 的唯一变更者（探针）的前置有界等待（§7.3 步骤 2）即该门禁的满足点。测试修正：v7 的"一 AP 未就绪时 shootdown"用例拆两个断言——①纯 shootdown（无映射变更）只等就绪者、成功；②AP 全部就绪后做实际映射变更（P1→P2）+ AP 读取验证（覆盖"就绪后无陈旧翻译"）。

### 6.5 跨核测试工作项

`ap_work` cmd 集合含：`WORK_READ64 {va, *out}`（读并记录）、`WORK_BARRIER {done}`。测试流程见 §8.2。

---

## 7. vmm_init 双后端

### 7.1 x86_64

行为不变（PMM walk + kernel_map 硬编码 + shootdown），迁 x86 backend。

### 7.2 aarch64 生产调用点

`boot/main.c` 中 `arch_boot_direct_map_init()` 成功后立即：

```c
int rc = arch_vmm_init();
if (rc) {
    kputs("FATAL: arch_vmm_init rc=-");
    kputu((uint64_t)(-(int64_t)rc));   /* kputu 单参数、无符号；负码拆符号+绝对值 */
    kputs("\n");
    arch_cpu_halt();
}
```

`arch_vmm_init`：读 `aarch64_read_ttbr1()`，校验 PA 非零/4K 对齐/< 1 TiB，`kernel_map = pa + ARCH_PAGE_OFFSET`。启动 selftest 断言 `kernel_map == (ttbr1 & BASE_MASK) + ARCH_PAGE_OFFSET`。

### 7.3 生产 shootdown 探针（正式镜像验收入口）

**scratch VA 选址（v5 确定）**：`SCRATCH_VA = ARCH_PAGE_OFFSET + 0x10000000`。理由：PA 0x10000000 落在 QEMU virt 设备间隙 `[0x0a000000, 0x40000000)` 内——不在 M1 映射集 R∪B∪D 中，该 VA 的 L2 slot 必为空，`map_4k_new` 不会撞占用项。探针启动时先断言 `arch_vmm_query_4k(SCRATCH_VA) == -ENOENT`（若非空立即 fail——防止未来内存布局变化静默破坏选址）。

boot 流程在 `arch_vmm_init` 成功后、AP online（`ap_work` 循环运行且 `percpu online` 就绪，§6.2b）后执行：

1. `kputs("M3-SHOOTDOWN-PROBE: START\n")`；
2. **有界等待 AP 就绪（v7）**：`smp_boot_aps()` 只等到更早的 `boot_online_set`——AP 之后还要处理 boot command、开 IRQ、发布 `ipi_ready`（§6.2b）。探针以 deadline（约 2 s，counter 计时）轮询 `load_acquire` 计数 `ipi_ready == PSCI 请求数`；达成后继续，**超时打印缺席 CPU 编号 → FAIL**（`M3-SHOOTDOWN-PROBE: FAIL ap-not-ready <ids>` 后 halt）。0 AP 就绪同样经此路径 FAIL；同时保留 `query_4k(SCRATCH_VA) == -ENOENT` 断言；
3. `P1 = alloc_4k_page()`（写 pattern A）、`P2 = alloc_4k_page()`（写 pattern B）；
4. BSP `arch_vmm_map_4k_new(kernel_map, P1, SCRATCH_VA, VM_KERNEL_RW)` → **向全部 `ipi_ready` AP**（双核 = 仅 CPU1；四核 = CPU1-3）发 `WORK_READ64(seq=n)` → 校验全部 == A；
5. `arch_vmm_update_4k(SCRATCH_VA → P2)`（BBM 类，窗口内 BSP 独占该 VA）→ shootdown → `WORK_READ64(seq=n+1)` 复读 == B（任一 AP 陈旧 TLB → 读到 A → fail）；
6. 清理与终态断言：`unmap_4k(SCRATCH_VA)` → `free_4k_page(P1)`、`free_4k_page(P2)` → `query_4k(SCRATCH_VA) == -ENOENT`（无遗留映射）；中间表页保留（F1 之前不回收，已文档化）；
7. `kputs("M3-SHOOTDOWN-PROBE: OK\n")`。

harness：正式镜像 QEMU 命令（与现有 aarch64 运行脚本同款，`-smp 2`——探针目标只有 CPU1），grep 串口 `M3-SHOOTDOWN-PROBE: OK`。多 AP（`-smp 4`）覆盖由 §8.2 的 selftest 工作项测试承担。

---

## 8. 测试与验收

测试落 **`hosttests/cases/`**；QEMU 用 `KERNEL_SELFTEST=1` 与**正式镜像**两种。

### 8.1 M2 矩阵

| 阶段 | 测试 | 期望 |
|----|----|----|
| RED→GREEN | `hosttests` slab 跨 16 size alloc/free/复用 + **幂等化后 x86 计数与改动前一致**（回归断言 using/free counts） | fail → pass |
| PREFLIGHT | 坏布局注入（slab 段推出 2 GiB / arena 跨空洞 / 溢出） | FATAL 路径命中 |
| 记账 | host test 用实际 j-loop 公式验证每 frame 恰好记一次（reservation 内的 slab frame 被跳过；reservation 外无 slab frame） | pass |
| QEMU | `KERNEL_SELFTEST=1` aarch64 -smp 2 启动 | 到 main；`end_of_struct ≤ 上界` 断言过 |
| 回归 | x86 hosttests + `systest_repeat.py` 5 连 | 5/5 |

### 8.2 M3 矩阵

| 阶段 | 测试 | 期望 |
|----|----|----|
| RED→GREEN | `hosttests`：12 合法组合 × leaf/block + 4 拒绝（`VM_NOCACHE` 无 `VM_NO_EXEC` × {RO,RW} × {USER,KERNEL}）+ 软件位 round-trip | pass |
| RED→GREEN | `hosttests`：split 后 512 PTE 逐条验 PA/权限/软件位；alloc 失败注入（返回 0）验证**原映射完好**；**已发布 root 调 split 返回 `-EPERM`**；锁内重读的 `-EAGAIN` 并发路径（mock 两次调用交错） | pass |
| RED→GREEN | `hosttests`：PROT_NONE 三态（query `-EPROT_NONE`+PA / unmap 返回 PA 不 free / update 暂存与恢复） | pass |
| RED→GREEN | `hosttests`：§4.4.3 四类替换协议各一组（权限变 / **内存类型变[纯描述符级，无 live 访问——语义位只有 Normal 与一种 NOCACHE，live Normal↔Device 切换需先设计无不兼容别名的物理区域，推迟并注明]** / PA 变 / 有效性翻转），含描述符中间态断言；**全新 L1 范围创建 block**（两级 ensure 触发 + ENOMEM 注入中间表保留可重试）；**ack 代数回绕**（构造 `gen=UINT32_MAX` → target=0，`!=` 等待不误判） | pass |
| RED→GREEN | `hosttests`：`arch_vmm_init` 后 kernel_map == TTBR1 root | pass |
| QEMU 单核 selftest | map/update/unmap/query × 4k/2m + split 单元 | 全过 |
| QEMU 多核 selftest | ① 工作项流程（含 seq 协议、**每项截止时间**与 **§6.2b 在线断言**：AP1 在目标集、其 `tlb_ack_gen` 递增——ack 确产生于 AP）：AP 读 P1（记录）→ BSP 换映 P2 + shootdown → AP 复读 == B（陈旧 TLB → 读到 A → fail）；② **部分掩码发送**：4 核下仅发 {1,2}，验证 CPU3 不 ack、1/2 ack；③ **一 AP 未就绪时纯 shootdown**（无映射变更）只等就绪者、成功返回；④ **全就绪后映射变更 + 全部 AP 读验证**（§6.4.7 门禁的正向验证）；⑤ 双 CPU 交错竞争同一 pt_lock 且一方持锁做 BBM 类 update 等 ack；⑥ 并发创建共享同一 L1 条目的两个不同 L2 slot 映射，恰建一张 L1 表、无表页泄漏；-smp 2 与 4（②④的多 AP 形态仅在 4） | 全过无死锁无超时 |
| QEMU **正式镜像** | §7.3 探针，`-smp 2`，grep `M3-SHOOTDOWN-PROBE: OK` | pass |
| 回归 | x86 `systest_repeat.py` 5 连；M1 16 组矩阵（arena 期望更新后）；ipi_test 不回归 | 5/5 |

### 8.3 验收门槛

- **M2**：hosttest GREEN（含 x86 计数回归）+ preflight FATAL 验证 + 记账单次性验证 + aarch64 selftest QEMU + x86 5/5。
- **M3**：hosttest 全 GREEN + aarch64 单/多核 selftest + **正式镜像探针** + x86 5/5。
- 不要求：aarch64 shell / aarch64 systest_repeat（M4+）。

---

## 9. 风险与遗留

**R7（v8 更新）**：锁与 IPI 交互死锁。**对策**：§5.4 纯自旋 + I1/I2 不变式 + 全序锁序（`pt_lock → pt_upper_lock → tlb_sd_lock`）+ 交错测试；**持锁等 ack 的安全性论证是通用的**（§5.4 通用规则），覆盖 BBM 类 update 与已发布 root 的 map/unmap 全部路径。残留风险：**正式构建无 I2 断言，审计是唯一防线**——M3.1 的审计门槛必须覆盖全部现存 irqsave→vmm 调用链，未来新增 caller 依赖 review 拦截。

**R13（v7 更新）**：已发布 root split 被移出 M3（`-EPERM`），但接口存在意味着未来 caller 可能拿到 `-EPERM` 才发现能力缺失；且 F10 的等级感知实现（BBM 检测 + nT + 发布后跨核失效）复杂度高。**对策**：M3 在 `split_block_2m` 文档注释与 `docs/memory.md` 显式标注范围；F10 有真实 caller（如内核 W^X 或 guard page）时才立项。

**R14（v7 重写）**：在线状态发布的顺序错误会造成两类故障——AP "标了 ipi_ready 却关着 IRQ"（死等）或 BSP online 窗口内 AP 跑 kmalloc（跳锁竞态）。**对策**：§6.2b 双状态协议明文规定顺序（BSP percpu init 提前到 `smp_boot_aps` 前 + online=1；AP 先开 IRQ 再标 ipi_ready）；显式目标列表发送使未就绪 AP 从不收 SGI；selftest 断言 ack 产生于 AP 的 `tlb_ack_gen` 递增 + "一 AP 未就绪时 shootdown"用例。

**R15（v6 新增）**：`VM_NOCACHE` 别名约束是 caller 契约而非后端强校验——违规 caller 不会得到错误码，只会得到属性不兼容别名（静默）。**对策**：backend 文档注释 + DEBUG 构建 PMM zone 交叉检查（可后续）；测试只用 Device 窗口 PA。

**R8**：SGI 与 GIC 嵌套。handler irqsave；GIC 优先级不动；多核测试覆盖。

**R9**：caller 排除不严。链接后 `nm` 检查无 `vma_*`/`uaccess_*`/fork 符号。

**R10**：AP 开 IRQ + 工作项循环是正式构建行为变更。正式镜像探针 + ipi_test 回归。

**R11（v4 新增）**：`percpu_t` 变更（`tlb_ack_gen`）触碰 144 B 钉死布局与 head.S stride——漏改会静默错位（见 memory：boot_percpu vs percpu_t 教训）。对策：`_Static_assert` 会在编译期拦截字段漂移；按 `percpu.h:75-85` 流程更新并全量重建。

**R12（v4 新增）**：arena 变大可能不再落在原 RAM 区间（容量选择回退/失败）。对策：M1 容量计算与失败注入重跑（§3.3）。

其余同前：全表 TLBI 性能（F2）、L3 不回收（F1）、软件位真机 PBHA（仅 QEMU 验证）、x86/aarch64 vmm_init 行为分裂（注释明示）。

**Follow-up**：F1 merge；F2 per-VA shootdown；F4 slab 递归 flag；F5/F6/F7 M4 用户地址空间与 vma/uaccess/task 重写；F8 x86 表页迁 `alloc_4k_page` 评估；**F10 已发布 root split 的等级感知实现**（按 `ID_AA64MMFR2_EL1.BBM` 支持等级选协议 + 发布后全范围跨核失效与完成等待；有真实 caller 时立项）；**F11（v8 新增）无不兼容别名的物理区域与 live 内存类型切换流程设计**。（v4 的 F9 已升格为 M3.1 完成门槛。）

---

## 10. 实施拆分

1. **M2.1**：slab 锁替换 + `slab_layout_compute()` + slab_init 标记幂等化 + x86 计数回归 hosttest。
2. **M2.2**：arena 公式链扩展（§3.2）+ preflight 前置检查 + §3.3 同步修改清单 + aarch64 编入 slab.c/删 stub + QEMU 验证。
3. **M3.1**：SGI 3 白名单 + `ipi_broadcast(mask)` aarch64 实现（显式目标列表）+ TLB handler + `secondary_idle` 工作项循环（含 §6.2 状态机/seq 协议，生产开 IRQ）+ **§6.2b 双状态发布协议（num_cpus / online / ipi_ready；BSP percpu init 提前到 smp_boot_aps 前）** + `tlb.c` 编入 + `percpu_t` 一次加 `ipi_ready`+`tlb_ack_gen`（PERCPU_DATA_SIZE/stride/重建）+ shootdown 串行化/目标快照/代数 ack（`!=` 等待）/超时 FATAL（x86 同步）+ 锁静态初始化（pt_locks/pt_upper_lock/tlb_sd_lock）+ **§5.4 审计门槛：枚举并消除全部 irqsave 锁内调用 vmm 变更 API 的现存链，清单入验收文档**。QEMU 多核（含正式镜像、含"一 AP 未就绪时 shootdown"用例）IPI 验证。**审计不通过不得进 M3.2。**
4. **M3.2**：公共 vmm.h 语义层 + x86 `pte.h` 私有层拆分（`PAGE_*` 迁移、x86-only 文件改 include、wrapper 保留释放逻辑）；x86 回归全过。
5. **M3.3**：page_table.c 受锁公开原语（软件位进出 + EPROT_NONE + block 编解码 + `pt_locks` 表 + `pt_upper_lock` + `walk_to_l2(create)` + I1/I2 断言）+ backend 4 KiB 全套；hosttest。
6. **M3.4**：block map/unmap + split（§5.3：**发布登记表 + M1 TTBR1 安装点登记**；未发布 root 原子 store；已发布 root `-EPERM`；boot 打印 `ID_AA64MMFR2_EL1.BBM`）；hosttest（含 512 项 `inherit()` 全等断言 + kernel_map 在登记表 → `-EPERM` 断言）+ 单核 QEMU。
7. **M3.5**：`arch_vmm_init` 生产调用点 + §6.4.7 共享映射变更门禁（ipi_ready 计数门禁 + DEBUG 断言）+ §7.3 探针（含有界等待/终态断言/工作项超时诊断）+ 多核工作项测试（含 §6.2b 在线断言、并发 L1 创建、ack 回绕、**全就绪后映射变更 + AP 读验证**）+ 正式镜像验收。
8. **M3.6**：总回归——x86 5/5、aarch64 单/多核、M1 矩阵（更新期望）、ipi_test、`nm` 符号检查。

每步独立 RED→GREEN；失败不进下一步。M3.1 仍居 M3.2 之前（shootdown 是后续验收依赖）。
