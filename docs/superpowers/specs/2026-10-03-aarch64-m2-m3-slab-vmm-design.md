---
title: OS01 aarch64 M2 Slab + M3 运行期 VMM 设计
created: 2026-10-03
type: spec
status: draft-for-review
tags: [osdev, aarch64, memory, slab, vmm, tlb]
---

# aarch64 M2 Slab 实装与 M3 运行期 VMM

## 1. 意图、基线与完成标准

依据 roadmap P2，M2 让 aarch64 kernel 从 `kmalloc()=NULL` 走到能在任意缓存大小上分配/释放；M3 让运行期既可映射/解除映射 4 KiB 也可创建/拆分 2 MiB block，并保留 x86_64 全部现行行为。M2+M3 是 M4 用户地址空间的前置；M4 不在本 spec。

**基线**：master @ `dabc9f0`（M1 merge 后的 vscode debug 修复）。前置依赖 M0 ✅ / M1 ✅ / Generic Timer ✅ / GICv2 Phase 1 ✅ / IPI fix ✅ / PMM arch-neutral ✅，均已 master。M1 BSP/AP 已共享同一 TTBR1 直映，`arch_boot_direct_map_*` 共用契约已就位；`kernel/arch/aarch64/memory/page_table.c` 提供 4 KiB leaf 编解码 + local TLBI（但只有 L3 leaf，未含 block 编码，未含拆/合）；`tlb_shootdown()` 接口已 arch-neutral（IPI 广播 + `tlb_wanted/ack` 协议），内部 `flush_tlb()` = CR3 重载；aarch64 端 `arch_flush_tlb_all` 当前是 weak default。

### M2 完成标准

- aarch64 build 编入 `kernel/memory/slab.c`，移除 `kernel/arch/aarch64/runtime/slab_stub.c`。
- `slab.c` 的 `slab_lock_acquire/release` 改用 `arch_local_irq_save/restore`（现有 facade，`kernel/include/arch/irq.h`）；`slab.c` 内不再出现 `pushfq` / `cli` / `sti` / `popfq` 字面。
- aarch64 kernel 启动走完 `slab_init()` 不 panic。
- host selftest 跨 16 个缓存大小（32 B … 1 MiB）kmalloc/kfree pass，含 free 后再分配复用验证。
- 初始化顺序契约"PMM 元数据 → M1 运行期直映 → Slab"落到 `docs/architecture.md` 或本 spec，并由 aarch64 启动路径隐式验证。

### M3 完成标准

- `kernel/include/memory/vmm.h` 暴露 arch-neutral 语义 flag（`VM_PRESENT` / `VM_WRITE` / `VM_USER` / `VM_NO_EXEC` / `VM_HUGE` / `VM_NOCACHE` / `VM_PROTNONE` / `VM_COW`），**位位置不再外泄**——任何 `PAGE_VALID=1` / `PAGE_WRITE=2` 之类 x86 PTE bit 在 vmm.h 里消失。
- 新增 `kernel/include/arch/<arch>/vmm_backend.h`（接口头）+ `kernel/arch/<arch>/memory/vmm_backend.c`（per-arch 实现）。
- x86_64 后端：从现有 `kernel/memory/vmm.c` 搬实现并按 `VM_*` 翻译原 `PAGE_*` flag。
- aarch64 后端：使用既有 `aarch64_pt_map_4k/unmap_4k` + 新增 `aarch64_pt_map_2m_block` / `aarch64_pt_split_block_2m` / `aarch64_pt_invalidate_block_local`。
- aarch64 `arch_vmm_init` 把 M1 root 直接注册为 `kernel_map`（不再走 PMM walk——M1 已建好直映）。
- aarch64 `arch_flush_tlb_all` 实现为 `tlbi vmalle1` + `dsb ish` + `isb`；IPI handler (`IPI_VECTOR_TLB`) 跑同一失效路径。
- host 边界测试：aarch64 PTE 编解码 round-trip（含 USER/KERNEL × RO/RW × EXEC/NOEXEC × NORMAL/DEVICE 共 8 种组合）+ split 后 L3 table 内容正确性 + 权限/软件位继承正确性。
- QEMU aarch64 多核（`-smp 2` 与 `-smp 4`）启动 + 跨 CPU 访问 4 KiB / 2 MiB 映射 + TLB shootdown 一致性。

### 不属于本 spec（明确边界）

- M4 用户 PGD / EL0 切换 / uaccess 故障恢复（roadmap 已列）。
- `vma.c` / `do_page_fault.c` 的 arch-neutral 部分重写（仅换 flag 名字与后端调用，不动 vma 语义）。
- Slab 算法优化（per-CPU cache / NUMA / page coloring）。
- ASLR / 用户栈随机化。

---

## 2. 已核实的约束

1. **`slab.c:36-55`** 现在 `slab_lock_acquire` 用 inline `pushfq; cli`，`slab_lock_release` 用 `sti`。`arch_local_irq_save/restore` 已存在（`kernel/include/arch/irq.h:55`），x86 用 pushfq+cli，aarch64 用 `mrs daif` + `msr daifset, #2`。可直接替换，无 API 变化。

2. **`slab_stub.c`** 是 4 个空函数（`slab_init` / `kmalloc` / `kfree` / `kzalloc` / `ksize`），签名与 `kernel/memory/slab.c` 完全一致。M1 spec §4.2 已明确"本项保留 aarch64 Slab stub"，意味着 M2 接手时 PMM 元数据 + M1 直映都已就绪，可直接调 `slab_init()`。

3. **`kernel/memory/vmm.c:115-117`** `vmm_init()` 把 `kernel_map = (uint64_t *)Phy_To_Virt(0x101000)`——x86_64 启动期 PML4 的物理地址硬编码。aarch64 端没有等价物；M1 的 TTBR1 root 必须从 `aarch64_read_ttbr1()` 拿，但 vmm 当前假设只有一个根。`vmm_init` 必须去 x86-ize。

4. **`vmm.c:69-90` `vmm_map_page`** 假设 L2 (PMD) 条目永远是 2 MiB block（`PAGE_HUGE` 必设）；aarch64 M1 直映就是 2 MiB block，但 M3 需要在 block 上做 4 KiB 子映射（split）才能支持后续 user page / kstack / guard page。`vmm_pt_walk:289-291` 已显式拒绝 `pmd[l2] & PAGE_HUGE`（"4KB operations must not walk into a 2MB PMD"）——这是 M3 split 的现成 hook 点。

5. **`vmm.h:33-39` 注释自承**："The bit positions above are still x86_64 PTE-format-specific; an aarch64 port will need to redefine these constants in an arch/<arch>/page.h header and re-route vmm.c through that."

6. **`aarch64_pt_map_4k:430-434`** 已有 `tlb_invalidate_local(va)` + `dsb ishst`；M3 后端只需在 `arch_vmm_map_4k` 成功路径上额外调 `tlb_shootdown()` 走 IPI 广播（多 CPU 时）。

7. **`tlb_shootdown()` 走 `IPI_VECTOR_TLB` + `tlb_wanted/ack` 协议**（`kernel/memory/tlb.c`），已 SMP-correct，arch-neutral 接口；aarch64 端需要在 IPI handler 里把 `flush_tlb()` 换成 `tlbi vmalle1`。

8. **GIC IPI 已通**（`docs/aarch64-ipi-fail-handoff-2026-09-26.md`）；TLB IPI 与 SGI IPI 共用 `ipi_broadcast`，handler 注册在 `IPI_VECTOR_TLB`。

9. **`aarch64_pt_range_accessible()`** 在 page_table.c 已实现（`495-525`），但 `arch/mmu.h::arch_user_range_accessible()` 仍返回 `false`（roadmap 提到）。M3 不动这条（属 M4），但 M3 的 `arch_vmm_map_4k` 之后会让 `arch_user_range_accessible` 的 aarch64 实现有真实页表可查——M4 启用，本 spec 不承接。

10. **现有 host test 套件**（`test/hosttest/memory/`）需要确认是否已有 slab 跨缓存测试；若不存在，本 spec 加一个 `kmalloc_sizes` host test。

---

## 3. M2 设计

### 3.1 改动清单

1. **`kernel/memory/slab.c`**：
   - 顶部增加 `#include <arch/irq.h>`。
   - `slab_lock_acquire`：返回类型由 `uint64_t` 改为 `arch_irq_state_t`；函数体第一句替换为 `arch_irq_state_t flags = arch_local_irq_save();`；其余不变；末尾 `return flags;`。
   - `slab_lock_release`：形参类型 `uint64_t flags` → `arch_irq_state_t flags`；函数体末尾 `if (flags & (1UL << 9)) __asm__ __volatile__("sti" ::: "memory");` → `arch_local_irq_restore(flags);`。
2. **`kernel/arch/aarch64/runtime/slab_stub.c`**：删除整个文件。
3. **aarch64 build profile**（`kernel/arch/aarch64/Makefile` 或等价 source group 入口）：把 `kernel/memory/slab.c` 加入 aarch64 `kernel-y`（参照 `docs/aarch64-libk-aarch64-closure-2026-09-24.md` 中 PMM 与 libk 接入 aarch64 build 的同款 source group 动作；arch source groups 之后 build 配置落在 `kernel/arch/aarch64/Makefile` 与 `kernel/Makefile` 协同位置）。`slab_stub.c` 退出 aarch64 编译源。
4. **`kernel/include/arch/irq.h` / per-arch 实现**：不动。

### 3.2 初始化顺序验证

按 M1 spec §4.2 已声明：aarch64 启动路径 "PMM 元数据 → M1 运行期直映 → Slab"，其中 PMM 末尾调 `slab_init()` 时 M1 已生效。本 spec 接受此顺序，**M2 期间不重排**；aarch64 启动日志增加一行 `slab_init: ok size=<bytes>` 作为隐式验证（slab 失败会 early printk 并 halt）。

### 3.3 M2 不动项

- 不改 slab 算法本身（per-CPU cache / NUMA / page coloring）；
- 不动 `kmalloc_create` 内 case 分支（只动锁）；
- 不优化 `kmalloc_creating` 递归 flag（已存在隐患，留 follow-up F4）。

---

## 4. vmm.h 抽象（核心架构决策）

### 4.1 语义 flag 命名（caller 视角）

旧 x86 PTE 名称 → 新 arch-neutral 语义：

| 旧名称 | 新名称 | 语义 |
|----|----|----|
| `PAGE_VALID` | `VM_PRESENT` | 表项有效 |
| `PAGE_WRITE` | `VM_WRITE` | 可写 |
| `PAGE_USER` | `VM_USER` | EL0 可达 |
| `PAGE_HUGE` | `VM_HUGE` | block 描述符（2 MiB / 1 GiB） |
| `PAGE_NO_EXEC` | `VM_NO_EXEC` | 不可执行 |
| `PAGE_PROTNONE` | `VM_PROTNONE` | 软标记：mprotect(PROT_NONE) 保留 PA |
| `PAGE_COW` | `VM_COW` | 软标记：fork 后共享只读 |
| `PAGE_CACHE_DISABLE` / `PAGE_WRITE_THROUGH` | `VM_NOCACHE` | 设备/UC（已合并） |
| `PAGE_DIRTY` / `PAGE_ACCESSED` | **删除** | arch 后端按需写 A/D 位，caller 不传 |
| `PAGE_GLOBAL` | **删除** | aarch64 用 ASID/nG；x86 全局页仅 kernel half 用，arch 内部决策 |
| `PAGE_PAT` | **删除** | aarch64 无 PAT |

`VM_*` 是 caller 用的位掩码常量（`VM_PRESENT | VM_WRITE | VM_USER`），具体位位置在 `arch/<arch>/vmm_backend.h` 定义。**caller 永不直接看位位置**。

### 4.2 接口（arch-neutral）

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
int   arch_vmm_map_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_map_2m(uint64_t *pgdir, uint64_t phys, uint64_t virt, uint32_t vm_flags);
int   arch_vmm_unmap_2m(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out);
int   arch_vmm_split_2m_to_4k(uint64_t *pgdir, uint64_t virt);
uint64_t *arch_vmm_pt_walk(uint64_t *pgdir, uint64_t virt, uint32_t vm_flags, int allocate);
void  arch_vmm_free_user_map(uint64_t *pgdir);

/* 旧符号保留 wrapper，调用对应 arch_vmm_*，避免一次性改全部 caller */
void      vmm_map_page(uint64_t *pgdir, uintptr_t pa, uintptr_t va, uint64_t flags);
uintptr_t vmm_unmap_page(uint64_t *pgdir, uintptr_t va);
mmap      vmm_alloc_map(void);
void      vmm_free_user_map(mmap pgdir);
```

旧 `PAGE_*` 名字保留为 deprecated alias（M3 末期一次性删 caller 后移除）。

### 4.3 软件位处理（最易踩坑）

x86 上 VM_PROTNONE 用 bit 9（PTE 软件位，硬件忽略），VM_COW 用 bit 10（同上）。aarch64 上 bit 9 = SH[0]、bit 10 = AF、bit 11 = nG——都不能给软件用。aarch64 的"软件可用位"在 bit [55:52] 和 bit [63:55]（descriptor 保留区）。

- x86 vmm_backend：`VM_PROTNONE` 翻译为描述符 bit 9，`VM_COW` 翻译为 bit 10。
- aarch64 vmm_backend：`VM_PROTNONE` 翻译为描述符 bit 55，`VM_COW` 翻译为 bit 56。
- caller 完全不知道位差异。
- aarch64 vmm_backend 写 PTE 时把硬件位（SH/AF/nG/AP/PXN/UXN/AttrIdx）按架构规范生成，软件位只覆盖 bit 55/56。
- `aarch64_pt_query_4k` 的 `decode_perm` 增加"软件位 → VM_PROTNONE / VM_COW"反向翻译（风险 R1）。

### 4.4 aarch64 vmm_backend 实现要点

1. `arch_vmm_init()`：从 `aarch64_read_ttbr1()` 读 M1 直映根，赋给 `kernel_map`；不重走 PMM walk；返回 0。详见 §7。
2. `arch_vmm_map_4k()`：**自动 split 语义**——若目标 L2 是 2 MiB block（VM_HUGE），先调 `arch_vmm_split_2m_to_4k(virt)`，再调 `aarch64_pt_map_4k(root, va, pa, vm_to_perm(vm_flags))`；成功路径：`tlb_invalidate_local(va)` + `tlb_shootdown()`。`vm_to_perm(vm_flags)` 是 `arch_vmm_backend.c` 内私有 helper，把 `VM_*` 语义位翻译为 aarch64 PTE AP/SH/AttrIndx/PXN/UXN + 软件位 bit 55/56；外部不可见。
3. `arch_vmm_unmap_4k()`：调 `aarch64_pt_unmap_4k`；`tlb_invalidate_local(va)` + `tlb_shootdown()`。
4. `arch_vmm_map_2m()`：L2 必须当前不在被占（V=0），直接生成 block descriptor 写入，调 `aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()`。
5. `arch_vmm_split_2m_to_4k(virt)`：分配一页 L3 table，遍历原 block 描述符的 512 个 PTE slot，按 `pa + i*4 KiB` 填 PTE，权限/软件位继承原 block；写入 pmd[l2]；触发 `tlb_invalidate_local(va)`。
6. `arch_vmm_pt_walk()`：若 L2 是 block 则不分裂，返回 NULL（同 `vmm_pt_walk:289-291` 语义）；caller 必须先 split。
7. `arch_vmm_free_user_map()`：遍历 L0[0..255] → L1 → L2，对 4 KiB PTE 走 free path，对 2 MiB block 走 free_2m_page（复用 `free_pages`）；回收中间表页。

### 4.5 x86_64 vmm_backend 实现要点

从现有 `kernel/memory/vmm.c` 搬实现，按 `VM_*` 翻译原 `PAGE_*` flag（位位置在 x86 与原 `PAGE_*` 完全一致，所以 x86 backend 内部 `vm_to_perm_x86 = 透传`）。原 `vmm.c` 退化为旧符号 wrapper + 共享逻辑（如 `tlb_shootdown` 调用）。

---

## 5. aarch64 block 编码 + split 细节

### 5.1 block 描述符（L2 PMD-equivalent）

`kernel/arch/aarch64/memory/page_table.c` 当前只生成 L3 leaf 描述符（bit 1 = 1）。M3 增加 block 描述符（bit 1 = 0，bit 0 = 1，PA[39:21] = block base）。需要的位定义沿用现有：

- `AARCH64_PT_DESC_VALID = 0x001`（bit 0）
- block 类型：bit 1 = 0（与 leaf/table 区分）
- PA：`bits [39:12]`（IPS=40 限制，PA[47:40] 必须为 0）
- 属性：AP / SH / AttrIndx / PXN / UXN / AF —— 与 leaf 同编码（仅消费 bit 0/1 不同）
- 软件位：bit 55 = VM_PROTNONE、bit 56 = VM_COW（与 leaf 一致）

新增 helper（独立函数，避免污染 `encode_perm` 的 leaf-only 假设）：

```c
static int encode_block_desc(uint32_t vm_flags, uint64_t pa, uint64_t *desc_out);
/* 与 encode_perm 共用 vm_flags → AP/SH/AttrIndx/PXN/UXN 翻译；
 * 但 valid bit 仅 0x001，bit 1 = 0；PA 字段放 bits [39:12]。 */
```

### 5.2 `aarch64_pt_map_2m_block(root, va, pa, vm_flags)`

- 校验：root_valid / va_canonical / `va & (2 MiB - 1) == 0` / `pa & (2 MiB - 1) == 0` / `pa < 1 TiB`。
- 调 `ensure_child_table` 走 L0/L1 拿到 `pmd`；若 `pmd[l2]` 已被占用（V=1）返回 EEXIST。
- `encode_block_desc` 生成描述符。
- `dsb_ishst(); pmd[l2] = desc; dsb_ishst();`
- 若 `is_active_root(root)`：`aarch64_pt_invalidate_block_local(va)` + `tlb_shootdown()` 走 IPI 广播。

### 5.3 `aarch64_pt_split_block_2m(root, va)`

- 校验同 map_2m。
- `ensure_child_table` 走到 `pmd`，读 `pmd[l2]`：必须是 block（V=1, bit 1 = 0）；否则 ECONFLICT。
- `alloc_4k_page()` 拿新 L3 table，zero_page。
- 遍历 i ∈ [0, 512)：
  - `pte[i] = (block_pa + i * 4 KiB) | inherit_from_block(block_desc)`；
  - `inherit_from_block` 把 block 的 AP/SH/AttrIndx/AF/PXN/UXN + 软件位（bit 55/56）原样移植（用户确认：保持一致语义）。
- `dsb_ishst(); pmd[l2] = (l3_pa) | encode_table_desc_flags; dsb_ishst();`（table 描述符：V=1, bit 1 = 1, 仅有 V/TYPE/PA，无 AP/AttrIdx 等，因为硬件忽略中间描述符的这些位，违反会触发 QEMU TCG 严格检查）。
- local TLBI（一次，因为 TLB 原 block entry 必须被踢出）+ `tlb_shootdown()`。
- 返回 OK，caller 接着写自己关心的那个 PTE slot。

### 5.4 merge 不纳入

用户确认：`aarch64_pt_merge_4k_to_2m`（L3 全空时回收表页合并 block）不纳入 M3，留 follow-up F1。caller 写完 PTE 后 L3 page 一直存在；后续 unmap 走 `free_4k_page(L3 page PA)` 是正确行为（代价：长生命周期内核页分配时占用的 L3 table 页不回收）。

---

## 6. aarch64 TLB shootdown

### 6.1 当前状态

- `kernel/memory/tlb.c::tlb_shootdown()` 是 arch-neutral 接口：单 CPU 走 `local_only` 直接 `flush_tlb()`；多 CPU 走 `tlb_wanted/ack` + `ipi_broadcast(IPI_VECTOR_TLB)`。
- `arch_flush_tlb_all()` 在 x86 是 CR3 重载；在 aarch64 当前是 weak default。

### 6.2 M3 改动

1. **`kernel/arch/aarch64/intr/tlb.c`**（或扩 `trap.c`）：注册 `IPI_VECTOR_TLB` handler：
   - 读 `percpu_data[cpu].tlb_wanted`（已经由发起方置 1）。
   - 调 `arch_local_irq_save()`。
   - `__asm__ __volatile__("tlbi vmalle1\n\t dsb ish\n\t isb" ::: "memory");` 全表失效。
   - `arch_local_irq_restore()`。
   - `percpu_data[cpu].tlb_ack = 1`。
   - 清 `tlb_wanted`。

2. **`arch_flush_tlb_all()` aarch64 实现**：在 `kernel/include/arch/aarch64/mmu.h` 或新 `kernel/arch/aarch64/memory/tlb.h` 提供 `static inline`：

   ```c
   static inline void arch_flush_tlb_all(void) {
       __asm__ __volatile__("tlbi vmalle1\n\t dsb ish\n\t isb" ::: "memory");
   }
   ```

   x86_64 端 `arch_flush_tlb_all` 保持 CR3 重载（不变）。

3. **per-VA local TLBI 已有**：`aarch64_pt_map_4k/unmap_4k` 内部 `tlb_invalidate_local(va)`（`tlbi vae1, X; dsb ish; isb`）。M3 vmm_backend 在 `arch_vmm_map_4k` / `arch_vmm_unmap_4k` 成功路径上保留这步，再调 `tlb_shootdown()` 走 IPI 广播（多 CPU 时）。

4. **block 映射的 local TLBI**：新增 `aarch64_pt_invalidate_block_local(va)`：

   ```c
   static inline void aarch64_pt_invalidate_block_local(uint64_t va) {
       __asm__ __volatile__(
           "tlbi vae1, %0\n\t"
           "dsb ish\n\t"
           "isb"
           :: "r"(va >> 12) : "memory");
   }
   /* 一次 TLBI 即覆盖整个 2 MiB block（TLB 条目粒度 = 块粒度）。 */
   ```

5. **IPI handler 死锁防护**：发起方持 `tlb_wanted=1` 期间，目标 CPU 可能在等同一个 spinlock——IPI handler 跑完才能 unlock。已有 `tlb_shootdown` 走的是 IPI + 自旋等 ack，与 slab_lock / vma_lock 等交互已验证（M1 systest 跑过）。aarch64 端 IPI handler 顺序"读 wanted → 失效 → 写 ack → 清 wanted"与 x86 一致，不引入新锁。

6. **块拆分后的 TLB 处理**：split 必须 `tlbi vae1, va>>12`（一次）踢掉原 block TLB entry；新 L3 table 的 4 KiB PTE 此时 V=0，TLB 自然不会命中；caller 写自己关心的 PTE 后再走一次 `arch_vmm_map_4k` 完整路径（local TLBI + shootdown）。

### 6.3 性能策略

用户确认：aarch64 `arch_flush_tlb_all` 采用全表失效（够用、简单）；per-VA shootdown（IPI 携带 wanted VA 列表）属 follow-up F2。

---

## 7. vmm_init 双后端

### 7.1 x86_64 端

原 `kernel/memory/vmm.c::vmm_init()` 行为不变，搬到 `kernel/arch/x86_64/memory/vmm_backend.c` 重命名为 `arch_vmm_init()`，实现透传 + `tlb_shootdown()`。`kernel_map = (uint64_t *)Phy_To_Virt(0x101000)` 的硬编码保留在 x86 backend 内部。

### 7.2 aarch64 端

```c
int arch_vmm_init(void)
{
    /* M1 已建好 TTBR1 直映；kernel_map 直接指向它即可。
     * 不再走 PMM walk——M1 把每个 zone frame 都已映射到 ARCH_PAGE_OFFSET+PA。 */
    uint64_t ttbr1 = aarch64_read_ttbr1();
    uint64_t pa = ttbr1 & AARCH64_TTBR_BASE_MASK;
    if (pa == 0 || pa >= AARCH64_PT_PA_LIMIT)
        return -EINVAL;
    kernel_map = (uint64_t *)(pa + ARCH_PAGE_OFFSET);
    /* M1 后所有 RAM 已可经 kernel_map + offset 访问，无需额外映射。 */
    return 0;
}
```

### 7.3 统一入口

```c
/* kernel/memory/vmm.c 或新 kernel/core/mm_init.c */
int vmm_init(void) { return arch_vmm_init(); }
```

caller 调 `vmm_init()` 不变；`arch_vmm_init` 的实际实现由 per-arch build profile 提供（weak default no-op 在 `arch/neutral/memory/`）。

---

## 8. 测试与验收

按 RED-before-GREEN 顺序。

### 8.1 M2 测试矩阵

| 阶段 | 测试 | 期望 | 文件 |
|----|----|----|----|
| RED | `test/hosttest/memory/test_slab_sizes.c`：16 size × 16 alloc/free pattern + free 后再 alloc 验复用 | 改动前 fail | 新增 |
| GREEN | 同上 | x86_64 host pass（slab_lock 路径触发，percpu_data[0].online=1） | 同 |
| BUILD | aarch64 build 编入 `kernel/memory/slab.c`、移除 `slab_stub.c` | build success | `kernel/arch/aarch64/Makefile` 调整 |
| RED → GREEN | aarch64 QEMU `KERNEL_SELFTEST=1` 启动到 main 提示符 | 启动成功 + `slab_init` 返回非 0 + 关键 kmalloc 子系统能创建结构 | QEMU launch script |
| 回归 | x86_64 `test/hosttest/memory/` 全部 + `systest_repeat.py` 5 连 | 5/5 pass | 既有 |

### 8.2 M3 测试矩阵

| 阶段 | 测试 | 期望 | 文件 |
|----|----|----|----|
| RED | `test/hosttest/memory/test_vmm_roundtrip.c`：8 种语义组合 × leaf/block × NORMAL/DEVICE + 软件位保留 | 改动前 fail | 新增 |
| RED | `test/hosttest/memory/test_vmm_split_merge.c`：构造 2 MiB block，split 后逐 PTE 验 PA + 权限 + 软件位 | 改动前 fail | 新增 |
| GREEN | x86_64 + aarch64 都 round-trip 通过 | 8 组合 × 2 类型 + 软件位全过 | 同 |
| RED | `test/hosttest/memory/test_vmm_init.c`：调 `arch_vmm_init`，x86 验证 kernel_map 非空 + PMM walk 完成；aarch64 验证 kernel_map = M1 TTBR1 root | x86 通过（既有路径），aarch64 改动前 fail | 新增 |
| QEMU 单核 | selftest 模式跑 `arch_vmm_map_4k / unmap_4k / map_2m / split_2m / pt_walk / free_user_map` 单元 | aarch64 -smp 1 全过 | selftest 命令 |
| QEMU 多核 | 跨 CPU 共享页测试：CPU A `arch_vmm_map_4k` 一个新页 → CPU B 立即 `*(volatile uint32_t *)va` 读到正确值；CPU B 在映射前的旧 TLB 状态被失效 | aarch64 -smp 2/4 全过 | selftest |
| 回归 | x86_64 systest_repeat.py 5 连 + aarch64 systest_repeat.py | x86 5/5；aarch64 ≥1 跑通 | 既有 |

### 8.3 验收门槛（PR 合并前必须）

- **M2**：hosttest 全部 GREEN + aarch64 selftest 启动成功 + x86_64 systest_repeat 5/5。
- **M3**：hosttest 全部 GREEN + aarch64 QEMU 单核+多核 selftest 通过 + x86_64 systest_repeat 5/5 + aarch64 systest_repeat 1/1。
- 任意 hosttest RED 不得 merge；任意 QEMU 失败必须 fix 后再跑。
- M1 已有验收（16 组 RAM/CPU/镜像矩阵、稀疏/容量耗尽/AP 无效 root 注入）维持不回归。

---

## 9. 风险与遗留

### 9.1 风险（实施期需关注）

**R1**：`aarch64_pt_query_4k` 的 `decode_perm` 不输出 VM_PROTNONE/VM_COW。当前 `decode_perm` 只解码 AP/SH/AttrIndx/EXEC → 8 种语义组合；软件位未翻译。M3 实施时需扩展 `decode_perm` 输出 bit 55/56 → VM_PROTNONE/VM_COW，并在 `aarch64_pt_query_4k` 把这些位传给 caller。否则 caller 读不到 PROT_NONE/COW 状态，影响 mprotect/fork 路径。**对策**：在 §5 block 描述符编码时一并补 `decode_perm`；host test 覆盖软件位 round-trip。

**R2**：aarch64 全表 TLBI 性能。每次 `arch_vmm_map_4k` / `unmap_4k` / `map_2m` / `split_2m` 都走 `tlbi vmalle1`（IPI 广播），所有 CPU 重载整个 TLB。x86 端 `flush_tlb()` = CR3 重载也走全表失效，二者在大致相同的 TLB churn 量级（aarch64 `tlbi vmalle1` 一次性失效所有 entry；x86 CR3 重载让所有 entry 失效）。M3 期间不会成为瓶颈（启动期映射次数有限），但 user 频繁 mmap/munmap（M4+）会成为热点。**对策**：M3 接受此代价；M4+ 阶段引入 per-VA shootdown（F2）作为优化。

**R3**：split 后 L3 table page 不回收。本 spec 不实现 merge；长生命周期 kernel 2 MiB block 拆出 L3 table 后该表页一直占着 PMM frame。极端情况下高频 fork + 大量 mmap 会积累；但 M3 不接 user mapping，影响仅限 kernel 内部 vmm_alloc_map 场景。**对策**：M3 不实现 merge（F1）；M4+ 评估"unmap 时检查 L3 全空 → 合并"后台清理。

**R4**：aarch64 VM 软件位 bit 55/56 在真硬件上的处理。QEMU TCG 对 descriptor 软件位的处理是宽松的（不会因软件位 ≠ 0 而 fault），但部分真硬件 silicon 可能把 bit [55:52] 用于 PBHA 或厂家自定义用途，导致 VM_COW/VM_PROTNONE 被硬件意外解释。**对策**：M3 阶段所有验证在 QEMU 完成；真硬件（P1 已有 USB 启动计划）出现"访问 PROT_NONE 页异常失败"或"COW 写时复制未触发"再追；不阻挡 M3 合并。

**R5**：aarch64 `arch_vmm_init` 把 M1 root 作为 kernel_map 后，x86 端 `vmm_init` 的 PMM walk 与 aarch64 端 no-op 行为分裂。未来 caller 假设 vmm_init 走完 PMM walk 时，aarch64 端行为不一致。**对策**：`vmm_init` 注释里写清"x86_64 walks PMM zones；aarch64 uses M1 direct map root"；M3 实施时在 `arch/mmu.h::vmm_init` 注释里也写。

**R6**：旧 `vmm.c` 内的 `get_next_level` 仍用 `calloc(1, PAGE_4K_SIZE)`（slab 路径）。M3 vmm_backend 接管后所有 caller 应走 `arch_vmm_pt_walk(create=1)`（内部用 `alloc_4k_page`）？原 vmm_pt_walk 用 `calloc` 是依赖 slab 已初始化；M3 aarch64 启动顺序是"PMM 元数据 → M1 → Slab"，但 `arch_vmm_pt_walk(create=1)` 在 Slab 之前被调（PMM 阶段）就 NPE。**对策**：所有 vmm_backend 的"分配表页"路径必须用 `alloc_4k_page`（PMM 路径，不依赖 Slab）；caller 限制"vmm 操作只能在 PMM 元数据初始化之后"。

### 9.2 Follow-up（不属本 spec）

- **F1**：merge_4k_to_2m（L3 全空时回收表页合并 block）—— 性能优化，属 M4+。
- **F2**：per-VA TLB shootdown（IPI 携带 wanted VA 列表，避免全表失效）—— 性能优化，属 M4+。
- **F3**：旧 `PAGE_*` 名字 deprecated alias 删干净 —— 一次性清理，caller 全部迁到 `VM_*` 后做。
- **F4**：slab.c 的 `kmalloc_creating` 递归 flag 改为 proper recursion detection（per-thread guard 或 KASan 风格）—— 已知隐患，与 M2 不绑。
- **F5**：`aarch64_pt_query_4k` 当前不返回软件位 → M3 期间补；补完后 caller（如 `vma.c` 的 mprotect、fork 的 COW 判定）可信赖返回值。
- **F6**：M4 用户地址空间（user PGD / EL0 切换 / uaccess 故障恢复 / `arch_user_range_accessible` 接通真实跨页权限检查）—— M3 完成后的下一 phase，roadmap 已列。

---

## 10. 实施拆分（建议，非强制）

M2 + M3 可以在同一份 plan 内拆为以下可独立验收子任务：

1. **M2.1**：`slab.c` 锁路径替换；host `test_slab_sizes.c` RED→GREEN。
2. **M2.2**：aarch64 build profile 加入 `slab.c` + 删除 `slab_stub.c`；QEMU 启动验证。
3. **M3.1**：vmm.h flag 重命名 + per-arch vmm_backend 接口骨架；x86_64 backend 搬实现；旧 `PAGE_*` deprecated alias；host `test_vmm_roundtrip.c` RED→GREEN。
4. **M3.2**：aarch64 vmm_backend 4 KiB 路径（`arch_vmm_map_4k` / `unmap_4k` / `pt_walk`）。
5. **M3.3**：aarch64 `aarch64_pt_map_2m_block` + `aarch64_pt_invalidate_block_local` + `arch_vmm_map_2m` / `unmap_2m`。
6. **M3.4**：aarch64 `aarch64_pt_split_block_2m` + `arch_vmm_split_2m_to_4k`；host `test_vmm_split_merge.c` RED→GREEN。
7. **M3.5**：aarch64 `arch_flush_tlb_all` + IPI handler；QEMU 多核跨 CPU TLB shootdown selftest。
8. **M3.6**：aarch64 `arch_vmm_init` 注册 M1 root；host `test_vmm_init.c` RED→GREEN。
9. **M3.7**：回归 — x86_64 systest_repeat 5 连 + aarch64 systest_repeat 1 跑通。

每个子任务独立 RED→GREEN→REFACTOR；失败不进入下一个。
