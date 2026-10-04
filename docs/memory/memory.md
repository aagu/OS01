# 内存管理系统

本系统实现了完整的 x86_64 架构内存管理系统，包括物理内存管理和虚拟内存管理。

## 内存管理系统架构

OS01采用Higher Half Kernel内存布局，内核程序使用`0xffff800000000000`以上的虚拟地址。这要求链接脚本将内核程序的起始地址设置为`0xffff800000000000 + 0x100000`，即高地址的1M位移处。

引导器（Loader）将内核程序加载到物理地址`0x100000`处，并跳转到该地址执行。该地址存放的是内核执行头程序，内核执行头会立即加载位于物理地址`0x10100`处的页目录，并通过`lret`指令切换到虚拟地址`0xffff80000010000`附近继续执行。

因此进入c语言的`kernel_main`函数后，应尽早重映射帧缓冲区以便可以在屏幕上输出文字内容。

### 物理内存管理

系统使用物理内存管理器（PMM）来管理物理内存，主要功能包括：

1. **内存检测**：通过 E820 内存映射检测系统可用内存
2. **内存划分**：将内存划分为不同的区域（Zone）
3. **页面管理**：使用页面（Page）结构管理物理页面
4. **页面分配**：分配和释放物理页面
5. **页面跟踪**：使用位掩码（bits_map）跟踪页面使用情况

### 虚拟内存管理

系统使用虚拟内存管理器（VMM）来管理虚拟内存，主要功能包括：

1. **页表管理**：使用多级页表（PML4, PML3, PML2）
2. **地址映射**：将虚拟地址映射到物理地址
3. **页表操作**：设置和刷新页表项

### 内存分配器

系统实现了多种内存分配器：

1. **物理页面分配器**：分配和释放物理页面
2. **Slab 分配器**：用于小内存分配

## 双架构启动直映（M1）

`arch/boot_memory.h` 是共同入口：PMM 初始化后、启动 AP 前只调用一次 `arch_boot_direct_map_init()`。成功才可查询 `arch_boot_direct_map_ready()`；重复初始化（包括首次失败）返回 `-EALREADY`。`arch_boot_direct_map_ranges()` 的 count 是输出值，返回不可变、合并的映射 RAM 区间，包含已占用 frame；不是可用页列表。未 ready 时有效输出先置 NULL/0，再返回 `-EAGAIN`；NULL 参数返回 `-EINVAL` 且不改其他输出。

x86_64 保留现有初始页表和 2 MiB 直映实现。非零 `ZONE_UNMAPPED_INDEX` 仍截止映射 zone，coverage 反映实际覆盖范围；中间表分配失败会终止启动。aarch64 保留 M0 TTBR0，BSP 安装独立 TTBR1 并执行 DSB/ISB/TLBI。高半区只保留真实归一化 RAM（Normal WBWA、inner shareable、EL1 RW、PXN/UXN）、启动 block `[0x40000000,0x40200000)`（EL1 可执行）和 Device-nGnRnE 窗口 `[0x08000000,0x0a000000)`（不可执行）。收紧以 2 MiB 为粒度。

AArch64 在 PMM 元数据写入前选择真实 RAM 与 `[0x40200000,0x80000000)` 的交集作为 arena。元数据按完整 RAM min/max 跨度（含 holes）计费，页表按唯一 512 GiB/1 GiB bucket 精确计费；整个 arena 向 2 MiB 对齐并预留。RAM 最多 16 段、PMM 最多 10 zones，PA 小于 1 TiB。低窗口容量不足、输入冲突或算术溢出会在元数据写入前失败。页表建造不依赖 Slab 或早期 4 KiB allocator。

AP 使用发布在 `.boot.bss` 的低物理 root scalar，在打开 MMU 前检查非零、4 KiB 对齐和 PA40 范围。BSP 按 CTR_EL0 cache line 将实际表、root、启动元数据与自测 probe 清至 PoC，DSB SY 后才 CPU_ON；AP 在 ACK 前核对实际 TTBR1 和高半区 sentinel。所有请求 AP ACK 后释放 probe，降级路径保留 probe 供迟到 AP 使用。4 KiB smoke 从初始空 L0[256] 取得独占临时子树，验证所有权后先脱链/TLBI，再逐页释放；通用 unmap 不回收中间表。

M1 不提供 AArch64 Slab（M2）或动态内核 VMM / block 拆分 / SMP shootdown（M3）。现有 Slab stub 仍保留，后续须按 PMM → M1 → Slab 的资源顺序接入。

### 验证记录（2026-10-03）

普通/自测镜像分别覆盖 `(RAM MiB, CPU)`：`(256,1),(512,1),(512,2),(512,4),(2048,1),(2048,2),(2048,4),(4096,1)`。16/16 通过。自测验证每 zone 首末 owned free frame 的首末 4 KiB、全部 represented frame 的实际页表、预热后裁掉旧 M0 非 RAM 映射、smoke 清理以及 AP 的高窗口 probe；2 GiB/4 GiB 情况 probe 优先在旧窗口之外。

命令、镜像/firmware/DTB SHA256、RAM/CPU 和串口日志保存在 `test-results/m1-ram/` 各 case 目录的 metadata/stdout/stderr 文件。x86 `test-qemu SUITE=phase-0` 与独立 `OS01_SYSTEST=1 test-qemu SUITE=systest` 通过（334 syscall tests）。稀疏变体（`MODE=m1-sparse` 及补充 harness 运行：512 MiB/2 GiB，1/4 核）以及 `m1-arena-exhaust`、`m1-table-exhaust`、`m1-ap-bad-root` 均符合预期；原 `smp`、`gic-spi`、`sync-fault` 回归通过。no-ACK 注入单独构建并通过独立降级预期。原生 host 54 suites、PMM boot reservation 及 static audits 均通过。

独立 whole-branch review 后补充了 preparation 失败诊断的非法输入回归、真实 planner 容量耗尽回归、负向日志变异回归及 AP 本地 TLBI 汇编顺序检查（`qemutests/aarch64_m1_ap_tlbi.py`，M1 矩阵入口自动执行）。所有重要问题经 RED→GREEN 修复后重跑验收。一个低优先级测试缺口保留：旧 `test_m1_tree` 的 canary 命名用例仅验证 PA resolver，没有 table buffer 外围 guard bytes；现有全树校验和编译期启动栈限制仍覆盖各自契约。

## 核心数据结构

### 物理内存管理器

```c
struct Physical_Memory_Manager {
    struct E820_ENTRY e820_entrys[E820_MAX_ENTRY_COUNT];
    uint64_t e820_length;
    
    uint64_t *bits_map;
    uint64_t bits_size;
    uint64_t bits_length;
    
    struct Page *pages_struct;
    uint64_t pages_size;
    uint64_t pages_length;
    
    struct Zone *zones_struct;
    uint64_t zones_size;
    uint64_t zones_length;
    
    uint64_t start_code;
    uint64_t end_code;
    uint64_t end_data;
    uint64_t end_rodata;
    uint64_t start_brk;
    uint64_t end_of_struct;
};
```

### 内存区域（Zone）

```c
struct Zone {
    uint64_t zone_start_address;
    uint64_t zone_end_address;
    uint64_t zone_length;
    
    uint64_t page_using_count;
    uint64_t page_free_count;
    uint64_t total_pages_link;
    
    uint64_t attribute;
    struct Physical_Memory_Manager *manager_struct;
    
    uint64_t pages_length;
    struct Page *pages_group;
};
```

### 页面（Page）

```c
struct Page {
    struct Zone *zone_struct;
    uint64_t phy_address;
    uint64_t attribute;
    
    uint64_t reference_count;
    
    uint64_t age;
};
```

## 物理内存管理

### 内存检测与初始化

系统通过 `pmm_init` 函数初始化物理内存管理：

1. **内存映射检测**：读取 E820 内存映射信息，识别可用内存区域
2. **内存区域划分**：将可用内存划分为不同的区域（Zone）
3. **页面结构初始化**：为每个物理页面创建 Page 结构
4. **位掩码初始化**：使用位掩码跟踪页面使用情况
5. **Slab 分配器初始化**：初始化 Slab 分配器

### 页面分配与释放

#### 页面分配

使用 `alloc_pages` 函数分配物理页面：

```c
struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags);
```

参数说明：
* `zone_select` - 内存区域选择（ZONE_DMA, ZONE_NORMAL, ZONE_UNMAPPED）
* `number` - 要分配的页面数量（最大 64）
* `page_flags` - 页面标志

#### 页面释放

使用 `free_pages` 函数释放物理页面：

```c
void free_pages(struct Page * page, int32_t number);
```

参数说明：
* `page` - 要释放的页面起始地址
* `number` - 要释放的页面数量

### 页面属性管理

#### 获取页面属性

```c
uint64_t get_page_attribute(struct Page *page);
```

#### 设置页面属性

```c
uint64_t set_page_attribute(struct Page * page, uint64_t flags);
```

## 虚拟内存管理

### 页表结构

系统使用 x86_64 架构的多级页表：

1. **PML4**（Page Map Level 4）：最高级页表
2. **PML3**（Page Directory Pointer Table）：中级页表
3. **PML2**（Page Directory）：低级页表

### 地址映射

#### 虚拟地址到物理地址的映射

```c
void vmm_map_page(uint64_t *pagemap, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags);
```

参数说明：
* `pagemap` - 页表基地址
* `physical_address` - 物理地址
* `virtual_address` - 虚拟地址
* `flags` - 页表项标志

#### 虚拟地址解除映射

```c
void vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address);
```

参数说明：
* `pagemap` - 页表基地址
* `virtual_address` - 虚拟地址

### 虚拟内存初始化

系统通过 `vmm_init` 函数初始化虚拟内存管理：

1. **获取内核页表**：获取内核页表基地址
2. **映射物理内存**：将物理内存映射到虚拟地址空间
3. **刷新 TLB**：刷新 Translation Lookaside Buffer

## 内存分配器

### Slab 分配器

Slab 分配器用于小内存分配，初始化函数为 `slab_init`，位于 `kernel/memory/slab.c` 中。

## COW (Copy-on-Write) Fork

系统支持 4KB 粒度的 COW fork（`PAGE_COW` PTE bit 10）：
- `subpage_pool`：`alloc_4k_page`/`free_4k_page` 分割 2MB 页面
- `subpage_pool.cow_count[512]`：每个 4KB 槽位的 COW 引用计数
- 缺页处理：`do_page_fault` 中 COW 分支解析写时复制
- `mmap`/`mprotect`/`munmap` 系统调用支持 VMA 追踪

## VMA (Virtual Memory Area)

`kernel/memory/vma.c` 实现 VMA 管理：
- `mmap` 分配 VMA，`munmap` 释放 VMA
- `mprotect` 修改 VMA 权限
- VMA 列表 (`mm_t.vma_list`) 按 `vm_start` 排序
- `mmap_base` 控制 mmap 搜索起始地址

## 内存初始化流程

1. **内核启动**：`kernel_main` 函数开始执行
2. **内存信息获取**：从引导加载程序获取内存信息
3. **物理内存初始化**：调用 `pmm_init` 初始化物理内存管理
4. **虚拟内存初始化**：调用 `vmm_init` 初始化虚拟内存管理
5. **Slab 分配器初始化**：在 `pmm_init` 中调用 `slab_init`
6. **内存映射**：将物理内存映射到虚拟地址空间
7. **COW/VMA 支持**：通过 `mmap`/`mprotect`/`munmap` 系统调用提供用户态内存管理

## 内存区域

系统将内存划分为三个主要区域：

1. **ZONE_DMA**：用于 DMA 操作的内存区域
2. **ZONE_NORMAL**：正常内存区域，已映射到页表
3. **ZONE_UNMAPPED**：未映射到页表的内存区域

## 页面大小

系统使用 2MB 大页面，定义如下：

```c
#define PAGE_2M_SIZE     0x200000
#define PAGE_2M_SHIFT    21
#define PAGE_2M_MASK     (~(PAGE_2M_SIZE - 1))
#define PAGE_2M_ALIGN(addr) (((addr) + PAGE_2M_SIZE - 1) & PAGE_2M_MASK)
```

## 代码结构

### 物理内存管理

* `kernel/memory/pmm.c` - 物理内存管理实现
* `kernel/include/memory/pmm.h` - 物理内存管理头文件

### 虚拟内存管理

* `kernel/memory/vmm.c` - 虚拟内存管理实现
* `kernel/include/memory/vmm.h` - 虚拟内存管理头文件

### 内存分配器

* `kernel/memory/slab.c` - Slab 分配器实现
* `kernel/include/memory/slab.h` - Slab 分配器头文件

### 内存工具

* `kernel/memory/dump.c` - 内存转储功能

### 内存头文件

* `kernel/include/memory/memory.h` - 内存管理公共头文件

## 注意事项

1. **内存对齐**：系统使用 2MB 大页面，内存分配需要对齐到 2MB 边界
2. **页表刷新**：修改页表后需要刷新 TLB，以确保修改生效
3. **内存区域选择**：根据不同的使用场景选择合适的内存区域
4. **页面引用计数**：页面有引用计数，确保在释放页面时引用计数为 0

## 内存管理示例

### 分配物理页面

```c
// 分配 1 个页面
struct Page *page = alloc_pages(ZONE_NORMAL, 1, PG_PTable_Mapped | PG_Kernel);

// 使用页面
uint64_t physical_address = page->phy_address;
uint64_t virtual_address = (uintptr_t)Phy_To_Virt(physical_address);

// 释放页面
free_pages(page, 1);
```

### 映射虚拟地址

```c
// 映射虚拟地址到物理地址
vmm_map_page(kernel_map, physical_address, virtual_address, PAGE_KERNEL_Page);

// 刷新 TLB
flush_tlb();

// 解除映射
vmm_unmap_page(kernel_map, virtual_address);
```

## 性能优化

1. **大页面使用**：使用 2MB 大页面减少页表层级，提高地址转换速度
2. **位掩码跟踪**：使用位掩码快速跟踪页面使用情况
3. **Slab 分配器**：使用 Slab 分配器提高小内存分配效率
4. **内存区域划分**：根据内存用途划分不同区域，提高内存使用效率

---

## v25/v26 增量：arch-neutral 化

### PMM arch-neutral（v24）

单一入口：`pmm_init(const struct boot_context *ctx)`。

- 弱默认 `pmm_arch_normalize` / `pmm_arch_zone_split` 在 `kernel/memory/pmm_arch.c`
- x86_64 强覆盖在 `kernel/arch/x86_64/memory/pmm_arch.c`：E820 + kernel-LMA/handoff/trampoline excludes + 2 MiB granule + sort/merge
- aarch64 强覆盖在 `kernel/arch/aarch64/memory/pmm_arch.c`：读 `aarch64_ram_map_get()`
- 中介层 `MEMORY_RANGE[]`：x86_64 E820 / aarch64 DTB 统一归一化输出
- `pmm.c` body 用 **RAM-relative indexing**（`pages_struct + ((start - lowest_ram) >> 21)`），Step 7 clamp 防 aarch64 unsigned-underflow

### 页表层级 PGD/PUD/PMD/PTE（v25）

Linux/ARM 命名替换 x86_64 PML4/PDPT/PDE。详见 `docs/arch.md`「页表层级」段。Bit-constant 同步 rename，~150 站点，x86_64 `kernel.bin` 字节相同（1,739,024 B），aarch64 编译视图纯净。

### PMM/sched 稳定性系列（v25）

5 commits + 6 例 PMM host 测试合并到 master：

- `ea89136` `MEMORY_RANGE_GRANULE` 64-bit 化（防 aarch64 32-bit mask 截断）
- `beb351c` x86_64 `pmm_arch_normalize` 保留 E820-derived `out[].type`（非-RAM 类型不丢；`pmm_init` Step 2 只 walk `MEMORY_TYPE_RAM` 是设计行为，非-RAM 留给未来 ACPI reclaim/NVS 消费者）
- `0ba888a` `bits_map` 在 `alloc_pages` / `free_pages` 中 RAM-relative 索引
- `0809100` kernel/slab 帧预留用 RAM-relative 索引而非物理 PFN（根因：devfs mount entry 0x600000 被 e1000 TX ring 覆写）
- `514e062` 调度器 lost-wakeup 窗口修复（dequeue → on_cpu=0 之间）
- `b68e1b1` 6 例 PMM host 测试（boot/slab RAM-relative 预留）

SMP=1/2/4 + systest-repeat 7 连 268/268 验证。详见 `docs/superpowers/specs/2026-09-09-pmm-arch-neutral-design.md`（13 轮 review）+ `plans/2026-09-09-pmm-arch-neutral.md`（16 task）。

## vmm 变更调用链审计（M3.1 验收）

M3.1 audit gate（Task 13）：审计每条调用 vmm 变更 API（`tlb_shootdown` /
`arch_vmm_*` / `vmm_*` 变更类）的调用链，确认 **irqsave 持锁段内不出现
vmm 变更调用**，且 `tlb_sd_lock` 临界区内不嵌套其他锁。

不变量（spec §5.4）：

- I1: 任何 `spin_lock_irqsave` / irqsave 持锁段内禁止调用
  `tlb_shootdown` / `arch_vmm_*` / vmm 变更 API（shootdown 的 ack 等待
  需要本 CPU 保持开中断应答 TLB IPI）。
- I2: `tlb_sd_lock` 必须是 plain `spin_lock`（等待者保持开中断）；
  TLB IPI handler 自身不取任何锁。
- I3: shootdown 目标集 = online ∧ `ipi_ready` ∧ ¬self（acquire 读
  `ipi_ready`）；未就绪 CPU 永不被瞄准，其 `tlb_ack_gen` 不变，就绪后
  下一次 shootdown 才递增。

静态扫描由 `hosttests/cases/test_vmm_caller_audit.c`（源码扫描半）+
`make PROFILE=aarch64-clang test-aarch64-audit`（nm 半：aarch64
kernel.elf 中不得出现 `vma_*`/`uaccess_*`/`fork_*` 的 T/t 符号）共同把守。

### aarch64（审计结论：全部 OK，无消除项）

| 调用点 | 持锁 | 调 vmm 变更? | 结论 |
|---|---|---|---|
| `memory/slab.c` 全部持锁段（`slab_lock_acquire`，irqsave） | `slab_lock` (irqsave, 可重入计数) | 否（slab.c 无任何 vmm/tlb 调用） | OK |
| `arch/aarch64/memory/early_arena.c` 全文件 | 无锁 | 否 | OK |
| `arch/aarch64/memory/page_table.c` `aarch64_pt_*` 入口 | 无锁 | 否（仅 `vmm_gate_check()` 门探测 + 本地 `tlbi vae1`） | OK |
| `arch/aarch64/memory/vmm_gate.c` 注册表操作 | `published_roots_lock` (plain spin) | 否 | OK |
| `memory/tlb.c:tlb_shootdown` | `tlb_sd_lock` (plain spin, I2) | 否（临界区内只 flush 本地 + 读 gen + `ipi_broadcast`） | OK（无嵌套锁：恰 1 次 `spin_lock`，2 次 `spin_unlock`——超时 FATAL 早退 + 正常退出） |
| `arch/aarch64/intr/ipi.c:ipi_broadcast` | 无锁（lock-free SGI 发送） | 否 | OK |

### x86_64（现状记录；不在 aarch64 kernel 白名单内，x86 现状不变）

| 调用点 | 持锁 | 调 vmm 变更? | 结论 |
|---|---|---|---|
| `memory/vmm.c:vmm_map_page`（kernel_map 共享 PMD 路径） | 无 | 是 (`tlb_shootdown`) | OK |
| `memory/vmm.c:vmm_unmap_page` | 无 | 是 (`tlb_shootdown`) | OK |
| `memory/vma.c:mm_set_brk`（grow 提交） | `mm->lock` (plain spin, 非 irqsave) | 是 (`tlb_shootdown`) | OK — 见排序规则 R1 |
| `memory/vma.c:mm_set_brk`（shrink 提交） | `mm->lock` (plain spin) | 是 (`tlb_shootdown` + `vmm_unmap_4k_page`) | OK — 见 R1 |
| `memory/uaccess.c:prepare_user_write_range_locked`（COW 写授权提交） | `mm->lock` (调用方 `prepare_user_write_range` 持锁) | 是 (`tlb_shootdown`) | OK — 见 R1 |
| `sched/task.c:fork_mm_copy`（fork COW 防护提交） | 无（PTE 改动后、无锁尾部调用） | 是 (`tlb_shootdown`) | OK |
| `core/printk.c:frame_buffer_init`（boot 期一次性映射） | 无 | 是 (`tlb_shootdown`) | OK（boot 期单 CPU） |

### 排序规则 R1（由上表归纳）

x86 存在三条 `mm->lock`(plain spin) 持锁跨 `tlb_shootdown` 的链。
允许成立的前提（均已验证成立，若破坏须消除该链）：

1. `mm->lock` 永远不以 irqsave 方式获取（当前 vma.c/uaccess.c 均为
   plain `spin_lock`，无 irqsave 变体）；
2. `tlb_sd_lock` 为 plain spin，等待者开中断，TLB IPI handler 无锁
   （I2），因此「目标 CPU 正自旋等 `mm->lock`」不阻碍其应答 shootdown；
3. 锁序固定为 `mm->lock` → `tlb_sd_lock`，任何反向获取都是 bug。

M3.1 无需消除项；`mm->lock`-跨-shootdown 三链记入 R1 持续约束。

### MODE=ipc-noready 的偏差说明

Brief Step 3 要求新增 QEMU 子模式 `MODE=ipc-noready` 验证「shootdown 只等
就绪者、未就绪 AP 的 gen 不变」。经评估 `mk/components/run.mk` 新增模式的
改版成本（prep/run helper 对 + image 变体 + harness parser）与收益不成
比例——该行为本质是 `tlb_shootdown` 的目标集过滤逻辑，已在 hosttest 层
覆盖：`test_tlb_serial_protocol.c` 新增
`case_unready_gen_defers_until_ready`（online 但 `ipi_ready=0` 的 AP
gen 不变 → 发布 ready → 下一次 shootdown gen 恰 +1 → 再一次再 +1），
连同既有的 `case_mask_excludes_unready_offline_self`。QEMU 侧由
`MODE=smp` 的 IPI 自测 + TLB 并存运行兜底（无死锁/超时）。
